using System.Text.Json.Nodes;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Core.Automation;

/// <summary>
/// Player profile: name, profession, level, XP, song stats, UID. Persisted
/// inside <c>settings.json</c> (no separate file). Mirrors Python
/// <c>character_profile.py</c> — backed by the <c>game_cache</c> +
/// <c>player_stats</c> sub-blobs.
/// </summary>
public sealed record CharacterProfile
{
    public string Username { get; init; } = "";
    public string Profession { get; init; } = "";
    public int Level { get; init; } = 1;
    public long Xp { get; init; }
    public int SongsPlayed { get; init; }
    public double PlayTime { get; init; }
    public string Uid { get; init; } = "";
}

/// <summary>
/// Profession metadata + level/XP math + persistence. Mirrors the public
/// API of Python <c>character_profile</c>: <c>load_profile</c>,
/// <c>save_profile</c>, <c>xp_for_level</c>, <c>calc_level</c>,
/// <c>add_song_xp</c>. The dialog/auto-detect bits are deliberately not
/// ported — the WPF profile editor is its own session.
/// </summary>
public static class CharacterProfileStore
{
    public const string GameCacheKey = "game_cache";
    public const string PlayerStatsKey = "player_stats";
    public static readonly string[] LegacyStatsKeys = { "midiplayer_stats" };

    /// <summary>id → display name. Source: StarResonanceDamageCounter <c>algo/packet.js</c>.</summary>
    public static readonly IReadOnlyDictionary<int, string> Professions = new Dictionary<int, string>
    {
        { 1,  "雷影剑士" }, { 2,  "冰魔导师" }, { 3,  "涤罪恶火·战斧" },
        { 4,  "青岚骑士" }, { 5,  "森语者" },   { 8,  "雷霆一闪·手炮" },
        { 9,  "巨刃守护者" }, { 10, "暗灵祈舞·仪刀" }, { 11, "神射手" },
        { 12, "神盾骑士" }, { 13, "灵魂乐手" },
    };

    public static IReadOnlyList<string> ProfessionList { get; } = Professions.Values.ToArray();

    /// <summary>skill_id → role tag (UI hint). Source: <c>SKILL_TO_ROLE_MAP</c>.</summary>
    public static readonly IReadOnlyDictionary<int, string> SkillToRole = new Dictionary<int, string>
    {
        { 1241, "射线" }, { 55302, "协奏" }, { 20301, "愈合" },
        { 1518, "惩戒" }, { 2306, "狂音" }, { 120902, "冰矛" },
        { 1714, "居合" }, { 44701, "月刃" }, { 220112, "鹰弓" },
        { 2203622, "鹰弓" }, { 1700827, "狼弓" }, { 1419, "空枪" },
        { 1418, "重装" }, { 2405, "防盾" }, { 2406, "光盾" },
        { 199902, "岩盾" },
    };

    public static int GetProfessionIdByName(string name)
    {
        foreach (var (id, n) in Professions) if (n == name) return id;
        return 0;
    }

    /// <summary>Load profile fields from settings.json's game_cache+player_stats.</summary>
    public static CharacterProfile Load(SettingsManager settings)
    {
        var profile = new CharacterProfile();
        var cache = settings.Get<JsonObject>(GameCacheKey);
        if (cache is not null)
        {
            profile = profile with
            {
                Username = (cache["player_name"]?.GetValue<string>() ?? "").Trim(),
                Profession = (cache["profession_name"]?.GetValue<string>() ?? "").Trim(),
                Uid = cache["player_id"]?.ToString() ?? "",
            };
            if (TryGetInt(cache["level_base"], out var lv) && lv > 0)
                profile = profile with { Level = lv };
        }

        var stats = LoadStats(settings);
        profile = profile with
        {
            Xp = stats.xp,
            SongsPlayed = stats.songs,
            PlayTime = stats.playTime,
        };
        return profile;
    }

    /// <summary>
    /// Persist profile back. Identity fields go into <c>game_cache</c> so
    /// the packet thread sees them; stats go into <c>player_stats</c> only
    /// when the caller actually changed them (avoids stomping on packet
    /// updates with default zeros).
    /// </summary>
    public static void Save(SettingsManager settings, CharacterProfile profile,
        bool persistStats = false)
    {
        var cache = settings.Get<JsonObject>(GameCacheKey) ?? new JsonObject();
        if (!string.IsNullOrWhiteSpace(profile.Username)) cache["player_name"] = profile.Username.Trim();
        if (!string.IsNullOrWhiteSpace(profile.Profession)) cache["profession_name"] = profile.Profession.Trim();
        if (profile.Level > 0) cache["level_base"] = profile.Level;
        if (!string.IsNullOrWhiteSpace(profile.Uid)) cache["player_id"] = profile.Uid.Trim();
        settings.Set(GameCacheKey, cache);

        if (persistStats || profile.Xp != 0 || profile.SongsPlayed != 0 || profile.PlayTime != 0)
        {
            var stats = new JsonObject
            {
                ["xp"] = profile.Xp,
                ["songs_played"] = profile.SongsPlayed,
                ["play_time"] = profile.PlayTime,
            };
            settings.Set(PlayerStatsKey, stats);
        }
        settings.Save();
    }

    /// <summary>Cumulative XP needed to reach <paramref name="level"/>.</summary>
    public static long XpForLevel(int level)
    {
        if (level <= 1) return 0;
        const int baseXp = 50;
        long total = 0;
        for (var lv = 2; lv <= level; lv++)
            total += (long)(baseXp * Math.Pow(lv, 1.3));
        return total;
    }

    /// <summary>Decompose total XP into (level, xpInLevel, xpForNext).</summary>
    public static (int Level, long InLevel, long ForNext) CalcLevel(long xp)
    {
        var level = 1;
        while (level < 999)
        {
            var next = XpForLevel(level + 1);
            if (xp < next)
            {
                var prev = XpForLevel(level);
                return (level, xp - prev, next - prev);
            }
            level++;
        }
        return (999, 0, 1);
    }

    /// <summary>Award XP for completing a song. Returns updated profile + level-up flags.</summary>
    public static (CharacterProfile Profile, bool LeveledUp, int OldLevel, int NewLevel)
        AddSongXp(CharacterProfile profile, double songDurationSec = 0)
    {
        var gain = 30L;
        if (songDurationSec > 30) gain += (long)(songDurationSec / 10);
        if (songDurationSec > 120) gain += 20;

        var newXp = profile.Xp + gain;
        var (newLevel, _, _) = CalcLevel(newXp);
        var updated = profile with
        {
            Xp = newXp,
            Level = newLevel,
            SongsPlayed = profile.SongsPlayed + 1,
            PlayTime = profile.PlayTime + songDurationSec,
        };
        return (updated, newLevel > profile.Level, profile.Level, newLevel);
    }

    private static (long xp, int songs, double playTime) LoadStats(SettingsManager settings)
    {
        var stats = settings.Get<JsonObject>(PlayerStatsKey);
        if (stats is null)
        {
            // Migrate from any legacy key on first read.
            foreach (var legacy in LegacyStatsKeys)
            {
                stats = settings.Get<JsonObject>(legacy);
                if (stats is not null)
                {
                    settings.Set(PlayerStatsKey, stats);
                    break;
                }
            }
        }
        if (stats is null) return (0, 0, 0);
        TryGetLong(stats["xp"], out var xp);
        TryGetInt(stats["songs_played"], out var songs);
        TryGetDouble(stats["play_time"], out var pt);
        return (xp, songs, pt);
    }

    private static bool TryGetInt(JsonNode? node, out int value)
    {
        value = 0;
        if (node is JsonValue jv)
        {
            if (jv.TryGetValue<int>(out value)) return true;
            if (jv.TryGetValue<long>(out var l)) { value = (int)l; return true; }
            if (jv.TryGetValue<string>(out var s) && int.TryParse(s, out value)) return true;
        }
        return false;
    }

    private static bool TryGetLong(JsonNode? node, out long value)
    {
        value = 0;
        if (node is JsonValue jv)
        {
            if (jv.TryGetValue<long>(out value)) return true;
            if (jv.TryGetValue<int>(out var i)) { value = i; return true; }
            if (jv.TryGetValue<string>(out var s) && long.TryParse(s, out value)) return true;
        }
        return false;
    }

    private static bool TryGetDouble(JsonNode? node, out double value)
    {
        value = 0;
        if (node is JsonValue jv)
        {
            if (jv.TryGetValue<double>(out value)) return true;
            if (jv.TryGetValue<long>(out var l)) { value = l; return true; }
            if (jv.TryGetValue<string>(out var s) &&
                double.TryParse(s, System.Globalization.CultureInfo.InvariantCulture, out value)) return true;
        }
        return false;
    }
}
