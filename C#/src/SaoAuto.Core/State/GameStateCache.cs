using System.Text.Json.Nodes;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Core.State;

/// <summary>
/// Bridges <see cref="SettingsManager"/> and <see cref="GameStateManager"/> for the
/// <c>game_cache</c> identity/HP/STA snapshot Python persists between launches.
/// Identity fields (name/level/profession/fight_point) are only overwritten
/// on save when the live value is non-empty/non-zero — same rule as Python.
/// </summary>
public static class GameStateCache
{
    public const string SettingsKey = SettingsKeys.GameCache;

    public static void Load(SettingsManager settings, GameStateManager states)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        if (states is null) throw new ArgumentNullException(nameof(states));

        var snapshot = settings.Snapshot();
        if (snapshot[SettingsKey] is not JsonObject cache) return;

        states.Update(s => s with
        {
            PlayerName = cache["player_name"]?.GetValue<string>() ?? s.PlayerName,
            LevelBase = ReadInt(cache, "level_base", s.LevelBase),
            LevelExtra = ReadInt(cache, "level_extra", s.LevelExtra),
            PlayerId = cache["player_id"]?.GetValue<string>() ?? s.PlayerId,
            SeasonExp = ReadInt(cache, "season_exp", s.SeasonExp),
            FightPoint = ReadInt(cache, "fight_point", s.FightPoint),
            HpCurrent = ReadInt(cache, "hp_current", s.HpCurrent),
            HpMax = ReadInt(cache, "hp_max", s.HpMax),
            HpPct = ReadDouble(cache, "hp_pct", s.HpPct),
            StaminaCurrent = ReadInt(cache, "stamina_current", s.StaminaCurrent),
            StaminaMax = ReadInt(cache, "stamina_max", s.StaminaMax),
            StaminaPct = ReadDouble(cache, "stamina_pct", s.StaminaPct),
            ProfessionId = ReadInt(cache, "profession_id", s.ProfessionId),
            ProfessionName = cache["profession_name"]?.GetValue<string>() ?? s.ProfessionName,
        });
    }

    public static void Save(SettingsManager settings, GameStateManager states)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        if (states is null) throw new ArgumentNullException(nameof(states));

        var snapshot = states.Snapshot;
        var existing = settings.Snapshot()[SettingsKey] as JsonObject ?? new JsonObject();

        WriteIdentityString(existing, "player_name", snapshot.PlayerName);
        WriteIdentityInt(existing, "level_base", snapshot.LevelBase);
        WriteIdentityInt(existing, "level_extra", snapshot.LevelExtra);
        WriteIdentityString(existing, "player_id", snapshot.PlayerId);
        existing["season_exp"] = snapshot.SeasonExp;
        WriteIdentityInt(existing, "fight_point", snapshot.FightPoint);
        existing["hp_current"] = snapshot.HpCurrent;
        existing["hp_max"] = snapshot.HpMax;
        existing["hp_pct"] = Math.Round(snapshot.HpPct, 4);
        existing["stamina_current"] = snapshot.StaminaCurrent;
        existing["stamina_max"] = snapshot.StaminaMax;
        existing["stamina_pct"] = Math.Round(snapshot.StaminaPct, 4);
        WriteIdentityInt(existing, "profession_id", snapshot.ProfessionId);
        WriteIdentityString(existing, "profession_name", snapshot.ProfessionName);

        settings.Set(SettingsKey, existing);
        settings.Save();
    }

    private static int ReadInt(JsonObject obj, string key, int fallback)
    {
        if (obj[key] is JsonValue v)
        {
            if (v.TryGetValue<int>(out var i)) return i;
            if (v.TryGetValue<double>(out var d)) return (int)d;
        }
        return fallback;
    }

    private static double ReadDouble(JsonObject obj, string key, double fallback)
    {
        if (obj[key] is JsonValue v)
        {
            if (v.TryGetValue<double>(out var d)) return d;
            if (v.TryGetValue<int>(out var i)) return i;
        }
        return fallback;
    }

    private static void WriteIdentityString(JsonObject obj, string key, string value)
    {
        if (!string.IsNullOrWhiteSpace(value))
        {
            obj[key] = value;
        }
    }

    private static void WriteIdentityInt(JsonObject obj, string key, int value)
    {
        if (value > 0)
        {
            obj[key] = value;
        }
    }
}
