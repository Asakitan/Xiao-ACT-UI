using System.Text.Json;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

[Collection("BossRaidProfileFactories")]
public class BossRaidProfileTests : IDisposable
{
    private readonly Func<string, string> _origId = BossRaidProfile.NewIdFactory;
    private readonly Func<string> _origNow = BossRaidProfile.UtcNowIsoFactory;
    private int _idCounter;

    public BossRaidProfileTests()
    {
        _idCounter = 0;
        BossRaidProfile.NewIdFactory = prefix => $"{prefix}_fake{++_idCounter:D3}";
        BossRaidProfile.UtcNowIsoFactory = () => "2026-05-06T00:00:00Z";
    }

    public void Dispose()
    {
        BossRaidProfile.NewIdFactory = _origId;
        BossRaidProfile.UtcNowIsoFactory = _origNow;
    }

    private static JsonElement Json(string raw) => JsonDocument.Parse(raw).RootElement;

    // ─ Coerce* ─────────────────────────────────────────────────────────

    [Theory]
    [InlineData("true", true)]
    [InlineData("false", false)]
    [InlineData("\"yes\"", true)]
    [InlineData("\"NO\"", false)]
    [InlineData("\"on\"", true)]
    [InlineData("\"off\"", false)]
    [InlineData("1", true)]
    [InlineData("0", false)]
    [InlineData("\"weird\"", true)] // falls back to default
    public void CoerceBoolHandlesVariants(string raw, bool expected)
    {
        Assert.Equal(expected, BossRaidProfile.CoerceBool(Json(raw), @default: true));
    }

    [Fact]
    public void CoerceIntClampsAndParsesString()
    {
        Assert.Equal(5, BossRaidProfile.CoerceInt(Json("\"5\""), 0));
        Assert.Equal(10, BossRaidProfile.CoerceInt(Json("100"), 0, maximum: 10));
        Assert.Equal(2, BossRaidProfile.CoerceInt(Json("1"), 0, minimum: 2));
        Assert.Equal(7, BossRaidProfile.CoerceInt(Json("null"), 7));
        Assert.Equal(3, BossRaidProfile.CoerceInt(Json("3.9"), 0));
    }

    [Fact]
    public void CoerceFloatClamps()
    {
        Assert.Equal(1.5, BossRaidProfile.CoerceFloat(Json("\"1.5\""), 0.0));
        Assert.Equal(10.0, BossRaidProfile.CoerceFloat(Json("99"), 0.0, maximum: 10.0));
        Assert.Equal(-1.0, BossRaidProfile.CoerceFloat(Json("-5"), 0.0, minimum: -1.0));
    }

    [Fact]
    public void CoerceStringTrimsAndHandlesScalars()
    {
        Assert.Equal("hi", BossRaidProfile.CoerceString(Json("\"  hi  \"")));
        Assert.Equal("", BossRaidProfile.CoerceString(Json("null")));
        Assert.Equal("3", BossRaidProfile.CoerceString(Json("3")));
    }

    // ─ Slugify / Mask ──────────────────────────────────────────────────

    [Theory]
    [InlineData("Boss 火焰_龙", "Boss_火焰_龙")]
    [InlineData("   ", "boss_raid_profile")]
    [InlineData("?!@#", "boss_raid_profile")]
    [InlineData("alpha-1", "alpha-1")]
    [InlineData(null, "boss_raid_profile")]
    public void SlugifyFilenameProducesExpected(string? input, string expected)
    {
        Assert.Equal(expected, BossRaidProfile.SlugifyFilename(input));
    }

    [Theory]
    [InlineData("", "")]
    [InlineData("abc", "***")]
    [InlineData("12345678", "********")]
    [InlineData("123456789", "1234...6789")]
    [InlineData("abcdefghijkl", "abcd...ijkl")]
    public void MaskTokenObscuresMiddle(string input, string expected)
    {
        Assert.Equal(expected, BossRaidProfile.MaskToken(input));
    }

    // ─ Defaults ────────────────────────────────────────────────────────

    [Fact]
    public void MakeDefaultProfileHasOnePhaseAndStableTimestamps()
    {
        var p = BossRaidProfile.MakeDefaultProfile();
        Assert.Equal("New Boss Raid", p.ProfileName);
        Assert.Equal("local", p.Source);
        Assert.Single(p.Phases);
        Assert.Equal("P1", p.Phases[0].Name);
        Assert.Equal("manual", p.Phases[0].Trigger.Type);
        Assert.Equal("2026-05-06T00:00:00Z", p.CreatedAt);
        Assert.Equal("2026-05-06T00:00:00Z", p.UpdatedAt);
        Assert.Equal(BossRaidProfile.SchemaVersion, p.SchemaVersion);
    }

    [Fact]
    public void DefaultConfigHasSiteUrlAndEmptyProfiles()
    {
        var c = BossRaidProfile.DefaultConfig();
        Assert.False(c.Enabled);
        Assert.Equal(BossRaidProfile.DefaultServerUrl, c.ServerUrl);
        Assert.Empty(c.Profiles);
        Assert.Equal("", c.ActiveProfileId);
        Assert.Equal(20, c.LastRemoteSearch.Query.PageSize);
    }

    // ─ Normalize* ──────────────────────────────────────────────────────

    [Fact]
    public void NormalizeTimelineConditionFallsBackToAlways()
    {
        var c = BossRaidProfile.NormalizeTimelineCondition(Json("{\"type\":\"unknown\"}"));
        Assert.NotNull(c);
        Assert.Equal("always", c!.Type);
        Assert.Equal(">=", c.Comparator);
    }

    [Fact]
    public void NormalizeTimelineClampsAndDefaults()
    {
        var t = BossRaidProfile.NormalizeTimeline(Json("{\"time_s\":99999, \"label\":\"\"}"));
        Assert.Equal(86400.0, t.TimeSeconds);
        Assert.Equal("Alert", t.Label);
        Assert.Equal("both", t.AlertType);
        Assert.StartsWith("tl_fake", t.Id);
    }

    [Fact]
    public void NormalizePhaseTriggerFallsBackToManual()
    {
        var trig = BossRaidProfile.NormalizePhaseTrigger(Json("{\"type\":\"bogus\",\"value\":7}"));
        Assert.Equal("manual", trig.Type);
        Assert.Equal(7.0, trig.Value);
    }

    [Fact]
    public void NormalizePhaseFillsNameAndKeepsTimelines()
    {
        var p = BossRaidProfile.NormalizePhase(
            Json("{\"timelines\":[{\"label\":\"Hi\"},{\"label\":\"Bye\"}]}"),
            fallbackIndex: 3);
        Assert.Equal("P3", p.Name);
        Assert.Equal(2, p.Timelines.Count);
    }

    [Fact]
    public void NormalizeAuthorSupportsAliases()
    {
        var a = BossRaidProfile.NormalizeAuthor(Json("{\"uid\":\"u1\",\"name\":\"Alice\",\"profession\":\"Bard\",\"profession_id\":4}"));
        Assert.Equal("u1", a.PlayerUid);
        Assert.Equal("Alice", a.PlayerName);
        Assert.Equal("Bard", a.ProfessionName);
        Assert.Equal(4, a.ProfessionId);
    }

    [Fact]
    public void NormalizeProfileEnsuresOnePhaseAndUpdatesTimestamp()
    {
        var p = BossRaidProfile.NormalizeProfile(Json("{}"));
        Assert.Single(p.Phases);
        Assert.Equal("P1", p.Phases[0].Name);
        Assert.Equal("local", p.Source);
        Assert.Equal("2026-05-06T00:00:00Z", p.UpdatedAt);
    }

    [Fact]
    public void NormalizeProfileRejectsUnknownSource()
    {
        var p = BossRaidProfile.NormalizeProfile(Json("{\"source\":\"piracy\"}"));
        Assert.Equal("local", p.Source);
    }

    [Fact]
    public void NormalizeConfigDropsOrphanActiveIdAndDefaultsToFirst()
    {
        var raw = Json("{\"profiles\":[{\"id\":\"a\"},{\"id\":\"b\"}],\"active_profile_id\":\"missing\"}");
        var c = BossRaidProfile.NormalizeConfig(raw);
        Assert.Equal(2, c.Profiles.Count);
        Assert.Equal("a", c.ActiveProfileId);
    }

    [Fact]
    public void NormalizeConfigUsesDefaultServerUrlWhenAbsent()
    {
        var c = BossRaidProfile.NormalizeConfig(Json("{}"));
        Assert.Equal(BossRaidProfile.DefaultServerUrl, c.ServerUrl);
    }

    // ─ Lookup / mutation ────────────────────────────────────────────────

    [Fact]
    public void UpsertProfileReplacesExistingAndCanActivate()
    {
        var c = BossRaidProfile.DefaultConfig();
        var a = BossRaidProfile.MakeDefaultProfile() with { Id = "a", ProfileName = "A" };
        var b = BossRaidProfile.MakeDefaultProfile() with { Id = "b", ProfileName = "B" };
        c = BossRaidProfile.UpsertProfile(c, a);
        c = BossRaidProfile.UpsertProfile(c, b, activate: true);
        Assert.Equal("b", c.ActiveProfileId);
        Assert.Equal(2, c.Profiles.Count);

        var aPrime = a with { ProfileName = "AA" };
        c = BossRaidProfile.UpsertProfile(c, aPrime);
        Assert.Equal(2, c.Profiles.Count);
        Assert.Equal("AA", BossRaidProfile.FindProfile(c, "a")!.ProfileName);
    }

    [Fact]
    public void DeleteProfileFallsBackActiveToFirst()
    {
        var c = BossRaidProfile.DefaultConfig();
        c = BossRaidProfile.UpsertProfile(c, BossRaidProfile.MakeDefaultProfile() with { Id = "a" }, activate: true);
        c = BossRaidProfile.UpsertProfile(c, BossRaidProfile.MakeDefaultProfile() with { Id = "b" });
        c = BossRaidProfile.DeleteProfile(c, "a");
        Assert.Single(c.Profiles);
        Assert.Equal("b", c.ActiveProfileId);
    }

    [Fact]
    public void CloneProfileGivesNewIdsAndCopySuffix()
    {
        var c = BossRaidProfile.DefaultConfig();
        var src = BossRaidProfile.NormalizeProfile(Json(
            "{\"id\":\"src\",\"profile_name\":\"Boss\",\"phases\":[{\"id\":\"p1\",\"timelines\":[{\"id\":\"tl1\",\"label\":\"x\"}]}]}"));
        c = BossRaidProfile.UpsertProfile(c, src);
        var (c2, cloned) = BossRaidProfile.CloneProfile(c, "src");
        Assert.NotNull(cloned);
        Assert.NotEqual("src", cloned!.Id);
        Assert.Equal("Boss Copy", cloned.ProfileName);
        Assert.Equal("local", cloned.Source);
        Assert.Null(cloned.RemoteId);
        Assert.NotEqual("p1", cloned.Phases[0].Id);
        Assert.NotEqual("tl1", cloned.Phases[0].Timelines[0].Id);
        Assert.Equal(2, c2.Profiles.Count);
    }

    [Fact]
    public void CloneMissingProfileReturnsConfigUnchanged()
    {
        var c = BossRaidProfile.DefaultConfig();
        var (c2, cloned) = BossRaidProfile.CloneProfile(c, "nope");
        Assert.Null(cloned);
        Assert.Same(c, c2);
    }

    [Fact]
    public void SummarizeProfileCountsPhasesAndTimelines()
    {
        var p = BossRaidProfile.NormalizeProfile(Json(
            "{\"phases\":[{\"timelines\":[{},{}]},{\"timelines\":[{}]}]}"));
        var s = BossRaidProfile.SummarizeProfile(p);
        Assert.Equal(2, s.PhaseCount);
        Assert.Equal(3, s.TimelineCount);
    }

    [Fact]
    public void ExportProfileJsonHasSchemaWrapperAndSnakeCase()
    {
        var p = BossRaidProfile.MakeDefaultProfile();
        var json = BossRaidProfile.ExportProfileJson(p);
        Assert.Contains("\"schema_version\"", json);
        Assert.Contains("\"profile\"", json);
        Assert.Contains("\"profile_name\"", json);
        Assert.Contains("\"author_snapshot\"", json);
    }

    [Fact]
    public void FindProfileReturnsNullForBlank()
    {
        var c = BossRaidProfile.DefaultConfig();
        Assert.Null(BossRaidProfile.FindProfile(c, ""));
        Assert.Null(BossRaidProfile.FindProfile(c, "   "));
    }
}
