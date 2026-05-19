using System.Collections.Immutable;
using System.Text.Json;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S73 — auto_key profile spec / normalization. Verifies coercion
/// parity with auto_key_engine.py (defaults, clamps, dropped
/// invalid conditions, fallback actions).
/// </summary>
public class Session73ProfileSpecTests
{
    private static JsonElement Js(string json) => JsonDocument.Parse(json).RootElement;

    private static readonly Func<string> SeqId = (Func<string>)(() =>
    {
        var i = ++_idCounter;
        return $"id{i:D12}";
    });
    private static int _idCounter;

    private static readonly Func<DateTimeOffset> FixedClock =
        () => new DateTimeOffset(2026, 5, 7, 12, 30, 0, TimeSpan.Zero);

    [Fact]
    public void Slugify_HandlesAlnumUnderscoreHyphenAndCJK()
    {
        Assert.Equal("Asuna_DPS", AutoKeyProfileSpec.Slugify("Asuna DPS"));
        Assert.Equal("a-b_c", AutoKeyProfileSpec.Slugify("a-b_c"));
        Assert.Equal("剑士", AutoKeyProfileSpec.Slugify("剑士"));
        Assert.Equal("剑士_pro", AutoKeyProfileSpec.Slugify("剑士 pro"));
        Assert.Equal("auto_key_profile", AutoKeyProfileSpec.Slugify(""));
        Assert.Equal("auto_key_profile", AutoKeyProfileSpec.Slugify("!!!"));
        Assert.Equal("a", AutoKeyProfileSpec.Slugify("___a___"));
    }

    [Fact]
    public void NewId_FormatsPrefixUnderscoreHex()
    {
        var id = AutoKeyProfileSpec.NewId("action", () => "deadbeef0123");
        Assert.Equal("action_deadbeef0123", id);
    }

    [Fact]
    public void UtcNowIso_FormatsZSuffix()
    {
        var s = AutoKeyProfileSpec.UtcNowIso(FixedClock);
        Assert.Equal("2026-05-07T12:30:00Z", s);
    }

    [Fact]
    public void MakeDefaultAction_ClampsSlotAndUsesDefaults()
    {
        var a = AutoKeyProfileSpec.MakeDefaultAction(slotIndex: 99, newId: () => "abc");
        Assert.Equal(9, a.SlotIndex);          // clamped
        Assert.Equal("9", a.Key);              // DEFAULT_ACTION_KEY_MAP[9]
        Assert.Equal("tap", a.PressMode);
        Assert.Equal(40, a.PressIntervalMs);
        Assert.Equal(80, a.HoldMs);
        Assert.Equal(800, a.MinRearmMs);
        Assert.Equal(120, a.PostDelayMs);
        Assert.Empty(a.Conditions);

        var lo = AutoKeyProfileSpec.MakeDefaultAction(slotIndex: -3, newId: () => "x");
        Assert.Equal(1, lo.SlotIndex);
    }

    [Fact]
    public void MakeDefaultProfile_PopulatesEngineAndOneAction()
    {
        var p = AutoKeyProfileSpec.MakeDefaultProfile(clock: FixedClock);
        Assert.Equal(1, p.SchemaVersion);
        Assert.Equal("local", p.Source);
        Assert.Null(p.RemoteId);
        Assert.Equal("2026-05-07T12:30:00Z", p.CreatedAt);
        Assert.Equal("2026-05-07T12:30:00Z", p.UpdatedAt);
        Assert.Equal(50, p.Engine.TickMs);
        Assert.True(p.Engine.RequireForeground);
        Assert.True(p.Engine.PauseOnDeath);
        Assert.Single(p.Actions);
        Assert.Equal(1, p.Actions[0].SlotIndex);
    }

    [Theory]
    [InlineData("hp_pct_gte", 0.5)]
    [InlineData("hp_pct_lte", 0.25)]
    [InlineData("sta_pct_gte", 0.9)]
    public void NormalizeCondition_NumericClamped(string type, double value)
    {
        var c = AutoKeyProfileSpec.NormalizeCondition(Js($"{{\"type\":\"{type}\",\"value\":{value}}}"));
        Assert.NotNull(c);
        var v = c switch
        {
            HpPctGteCondition x => x.Value,
            HpPctLteCondition x => x.Value,
            StaPctGteCondition x => x.Value,
            _ => -1,
        };
        Assert.Equal(value, v);

        var clipHi = AutoKeyProfileSpec.NormalizeCondition(Js($"{{\"type\":\"{type}\",\"value\":2.5}}"));
        var clipLo = AutoKeyProfileSpec.NormalizeCondition(Js($"{{\"type\":\"{type}\",\"value\":-0.5}}"));
        Assert.NotNull(clipHi); Assert.NotNull(clipLo);
    }

    [Fact]
    public void NormalizeCondition_BurstReadyAndInCombatBoolDefaults()
    {
        var br = AutoKeyProfileSpec.NormalizeCondition(Js("{\"type\":\"burst_ready_is\"}"));
        Assert.IsType<BurstReadyIsCondition>(br);
        Assert.False(((BurstReadyIsCondition)br!).Value);

        var ic = AutoKeyProfileSpec.NormalizeCondition(Js("{\"type\":\"in_combat_is\"}"));
        Assert.IsType<InCombatIsCondition>(ic);
        Assert.True(((InCombatIsCondition)ic!).Value);  // in_combat default = true
    }

    [Fact]
    public void NormalizeCondition_SlotStateLowercasedDefaultReady()
    {
        var c = AutoKeyProfileSpec.NormalizeCondition(Js("{\"type\":\"slot_state_is\",\"slot_index\":5,\"state\":\"COOLDOWN\"}"));
        var s = Assert.IsType<SlotStateIsCondition>(c);
        Assert.Equal(5, s.SlotIndex);
        Assert.Equal("cooldown", s.State);

        var def = AutoKeyProfileSpec.NormalizeCondition(Js("{\"type\":\"slot_state_is\",\"slot_index\":1}"));
        Assert.Equal("ready", ((SlotStateIsCondition)def!).State);
    }

    [Fact]
    public void NormalizeCondition_UnknownTypeReturnsNull()
    {
        Assert.Null(AutoKeyProfileSpec.NormalizeCondition(Js("{\"type\":\"made_up\"}")));
        Assert.Null(AutoKeyProfileSpec.NormalizeCondition(Js("{}")));
        Assert.Null(AutoKeyProfileSpec.NormalizeCondition(Js("\"not-an-object\"")));
    }

    [Fact]
    public void NormalizeAction_AppliesDefaultsAndClamps()
    {
        var a = AutoKeyProfileSpec.NormalizeAction(Js("{}"), fallbackSlot: 3, newId: () => "g");
        Assert.Equal(3, a.SlotIndex);
        Assert.Equal("3", a.Key);
        Assert.Equal("tap", a.PressMode);
        Assert.True(a.Enabled);

        var clamped = AutoKeyProfileSpec.NormalizeAction(
            Js("{\"slot_index\":99,\"press_count\":99,\"press_mode\":\"weird\",\"hold_ms\":50000,\"key\":\"q\"}"),
            newId: () => "g2");
        Assert.Equal(9, clamped.SlotIndex);
        Assert.Equal(20, clamped.PressCount);
        Assert.Equal("tap", clamped.PressMode);
        Assert.Equal(10_000, clamped.HoldMs);
        Assert.Equal("Q", clamped.Key);  // upper-cased
    }

    [Fact]
    public void NormalizeAction_DropsInvalidConditions_KeepsValidOnes()
    {
        var json = """
            {"slot_index":1,"conditions":[
                {"type":"hp_pct_lte","value":0.4},
                {"type":"unknown"},
                "string-not-object",
                {"type":"burst_ready_is","value":true}
            ]}
            """;
        var a = AutoKeyProfileSpec.NormalizeAction(Js(json), newId: () => "x");
        Assert.Equal(2, a.Conditions.Length);
        Assert.IsType<HpPctLteCondition>(a.Conditions[0]);
        Assert.IsType<BurstReadyIsCondition>(a.Conditions[1]);
    }

    [Fact]
    public void NormalizeProfile_EmptyActions_GetsOneDefault()
    {
        var p = AutoKeyProfileSpec.NormalizeProfile(Js("{}"), clock: FixedClock, newId: () => "z");
        Assert.Single(p.Actions);
        Assert.Equal("local", p.Source);
        Assert.Equal(1, p.SchemaVersion);
        Assert.Equal("2026-05-07T12:30:00Z", p.UpdatedAt);
    }

    [Fact]
    public void NormalizeProfile_PreservesIdAndCreatedAt_RewritesUpdatedAt()
    {
        var json = """
            {"id":"profile_keepme","created_at":"2025-01-01T00:00:00Z",
             "profile_name":"My Build","source":"downloaded","remote_id":"r42",
             "engine":{"tick_ms":5,"require_foreground":false,"pause_on_death":false},
             "actions":[{"slot_index":1},{"slot_index":2}]}
            """;
        var p = AutoKeyProfileSpec.NormalizeProfile(Js(json), clock: FixedClock, newId: () => "n");
        Assert.Equal("profile_keepme", p.Id);
        Assert.Equal("My Build", p.ProfileName);
        Assert.Equal("downloaded", p.Source);
        Assert.Equal("r42", p.RemoteId);
        Assert.Equal("2025-01-01T00:00:00Z", p.CreatedAt);
        Assert.Equal("2026-05-07T12:30:00Z", p.UpdatedAt);
        Assert.Equal(10, p.Engine.TickMs);  // clamped from 5 → 10
        Assert.False(p.Engine.RequireForeground);
        Assert.Equal(2, p.Actions.Length);
        Assert.Equal(2, p.Actions[1].SlotIndex);
    }

    [Fact]
    public void NormalizeProfile_UnknownSourceFallsBackToLocal()
    {
        var p = AutoKeyProfileSpec.NormalizeProfile(
            Js("{\"source\":\"made-up\"}"), clock: FixedClock, newId: () => "x");
        Assert.Equal("local", p.Source);
    }

    [Fact]
    public void NormalizeProfile_AuthorFallsBackThenSnapshotOverrides()
    {
        var fallback = new AuthorSnapshot("uid1", "Kirito", 3, "Swordsman");
        var p1 = AutoKeyProfileSpec.NormalizeProfile(Js("{}"), authorFallback: fallback, clock: FixedClock, newId: () => "a");
        Assert.Equal("Kirito", p1.AuthorSnapshot.PlayerName);
        Assert.Equal(3, p1.ProfessionId);

        var json = "{\"author_snapshot\":{\"player_name\":\"Asuna\",\"profession_id\":7,\"profession_name\":\"Rapier\"}}";
        var p2 = AutoKeyProfileSpec.NormalizeProfile(Js(json), authorFallback: fallback, clock: FixedClock, newId: () => "b");
        Assert.Equal("Asuna", p2.AuthorSnapshot.PlayerName);
        Assert.Equal(7, p2.ProfessionId);
        Assert.Equal("Rapier", p2.ProfessionName);
    }

    [Fact]
    public void NormalizeAuthor_AcceptsAltKeys()
    {
        var a = AutoKeyProfileSpec.NormalizeAuthor(
            Js("{\"uid\":\"u9\",\"name\":\"Lyfa\",\"profession\":\"Mage\"}"));
        Assert.Equal("u9", a.PlayerUid);
        Assert.Equal("Lyfa", a.PlayerName);
        Assert.Equal("Mage", a.ProfessionName);
    }

    [Fact]
    public void DefaultsConstants_MatchPython()
    {
        Assert.Equal(1, AutoKeyProfileSpec.SchemaVersion);
        Assert.Equal("http://doi.sakisense.top:15538", AutoKeyProfileSpec.DefaultServerUrl);
        Assert.Equal("5", AutoKeyProfileSpec.DefaultKeyForSlot(5));
    }
}
