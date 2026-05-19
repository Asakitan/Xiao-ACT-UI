using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.Automation;

public class CharacterProfileStoreTests
{
    private static SettingsManager NewSettings()
    {
        var path = Path.Combine(Path.GetTempPath(), $"sao_profile_{Guid.NewGuid():N}.json");
        return new SettingsManager(path);
    }

    [Fact]
    public void XpForLevel_MatchesPythonFormula()
    {
        Assert.Equal(0, CharacterProfileStore.XpForLevel(1));
        // python: int(50 * 2**1.3) == 123
        Assert.Equal(123, CharacterProfileStore.XpForLevel(2));
        // 1→2 + 2→3 == 123 + int(50*3^1.3) == 123 + 208
        Assert.Equal(123 + 208, CharacterProfileStore.XpForLevel(3));
        Assert.True(CharacterProfileStore.XpForLevel(20) > CharacterProfileStore.XpForLevel(10));
    }

    [Fact]
    public void CalcLevel_DecomposesXp()
    {
        var (lv, inLv, forNext) = CharacterProfileStore.CalcLevel(0);
        Assert.Equal(1, lv); Assert.Equal(0, inLv); Assert.True(forNext > 0);

        var (lv2, inLv2, _) = CharacterProfileStore.CalcLevel(123);
        Assert.Equal(2, lv2); Assert.Equal(0, inLv2);

        var (lv3, _, _) = CharacterProfileStore.CalcLevel(122);
        Assert.Equal(1, lv3);
    }

    [Fact]
    public void AddSongXp_LevelsUpAndAccumulates()
    {
        var p = new CharacterProfile { Level = 1 };
        var (p2, leveled, _, newLv) = CharacterProfileStore.AddSongXp(p, songDurationSec: 60);
        Assert.Equal(1, p2.SongsPlayed);
        Assert.Equal(60, p2.PlayTime);
        Assert.True(p2.Xp >= 30 + 6); // 30 base + 60/10
        Assert.Equal(p2.Level, newLv);
        // 30+6 == 36 < 123 → still level 1
        Assert.False(leveled);
    }

    [Fact]
    public void AddSongXp_LongSongCrossesLevel()
    {
        var p = new CharacterProfile { Level = 1, Xp = 122 };
        var (p2, leveled, oldLv, newLv) = CharacterProfileStore.AddSongXp(p, songDurationSec: 0);
        Assert.True(leveled);
        Assert.Equal(1, oldLv);
        Assert.Equal(2, newLv);
        Assert.Equal(2, p2.Level);
    }

    [Fact]
    public void SaveLoad_RoundTripsIdentityAndStats()
    {
        var s = NewSettings();
        var profile = new CharacterProfile
        {
            Username = "BenCat",
            Profession = "灵魂乐手",
            Level = 7,
            Xp = 4242,
            Uid = "12345",
            SongsPlayed = 9,
            PlayTime = 720.5,
        };
        CharacterProfileStore.Save(s, profile, persistStats: true);

        var s2 = new SettingsManager(s.Path);
        var loaded = CharacterProfileStore.Load(s2);
        Assert.Equal("BenCat", loaded.Username);
        Assert.Equal("灵魂乐手", loaded.Profession);
        Assert.Equal(7, loaded.Level);
        Assert.Equal(4242, loaded.Xp);
        Assert.Equal("12345", loaded.Uid);
        Assert.Equal(9, loaded.SongsPlayed);
        Assert.Equal(720.5, loaded.PlayTime);

        File.Delete(s.Path);
    }

    [Fact]
    public void Save_DoesNotStompPlayerStatsWhenStatsAreZero()
    {
        var s = NewSettings();
        var seed = new JsonObject { ["xp"] = 9999, ["songs_played"] = 5, ["play_time"] = 100.0 };
        s.Set(CharacterProfileStore.PlayerStatsKey, seed);
        s.Save();

        var profile = new CharacterProfile { Username = "猫", Level = 3 };
        CharacterProfileStore.Save(s, profile); // persistStats default false, all stats zero

        var stats = s.Get<JsonObject>(CharacterProfileStore.PlayerStatsKey)!;
        Assert.Equal(9999, (int)stats["xp"]!);

        File.Delete(s.Path);
    }

    [Fact]
    public void Load_MigratesLegacyMidiplayerStatsKey()
    {
        var s = NewSettings();
        var legacy = new JsonObject { ["xp"] = 555, ["songs_played"] = 3, ["play_time"] = 42.0 };
        s.Set("midiplayer_stats", legacy);
        s.Save();

        var loaded = CharacterProfileStore.Load(s);
        Assert.Equal(555, loaded.Xp);
        Assert.Equal(3, loaded.SongsPlayed);
        Assert.Equal(42.0, loaded.PlayTime);

        File.Delete(s.Path);
    }

    [Fact]
    public void GetProfessionIdByName_FindsKnownAndReturnsZeroForUnknown()
    {
        Assert.Equal(13, CharacterProfileStore.GetProfessionIdByName("灵魂乐手"));
        Assert.Equal(0, CharacterProfileStore.GetProfessionIdByName("nope"));
    }
}
