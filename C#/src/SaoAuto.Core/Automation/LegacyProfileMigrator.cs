using System.Text.Json.Nodes;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Core.Automation;

/// <summary>
/// S89 — One-shot migrator from the legacy <c>player_profile.json</c>
/// (pre-2026.04 / midiplayer-era stand-alone profile file) into the
/// unified <c>settings.json</c> game_cache + player_stats sub-blobs.
///
/// Mirrors Python <c>character_profile._migrate_legacy_profile_once</c>
/// (character_profile.py 165–223). Identity fields only fill empty
/// slots in <c>game_cache</c> (the live packet thread is the authority);
/// stats fields only land if the canonical <c>player_stats</c> blob
/// doesn't already have them. The legacy file is deleted on a
/// successful save so subsequent calls are file-system no-ops.
/// </summary>
public static class LegacyProfileMigrator
{
    private static readonly string[] StatsFields = { "xp", "songs_played", "play_time" };

    public sealed record MigrationResult(
        bool LegacyFileFound,
        bool SettingsChanged,
        bool LegacyFileDeleted,
        string? Error);

    /// <summary>
    /// Run the migration. Safe to call repeatedly: when the legacy file
    /// is gone (deleted by a previous successful run, or never existed),
    /// returns <c>(false, false, false, null)</c>.
    /// </summary>
    /// <param name="settings">Live settings store; mutated + saved on a successful merge.</param>
    /// <param name="legacyFilePath">Absolute path to the legacy <c>player_profile.json</c>.</param>
    /// <param name="logger">Optional logger.</param>
    public static MigrationResult Migrate(
        SettingsManager settings,
        string legacyFilePath,
        ILogger? logger = null)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        if (string.IsNullOrEmpty(legacyFilePath)) throw new ArgumentNullException(nameof(legacyFilePath));
        var log = logger ?? NullLogger.Instance;

        if (!File.Exists(legacyFilePath))
        {
            return new MigrationResult(false, false, false, null);
        }

        JsonObject legacy;
        try
        {
            using var stream = File.OpenRead(legacyFilePath);
            var node = JsonNode.Parse(stream);
            if (node is not JsonObject obj)
            {
                // Python: not isinstance(legacy, dict) → silently skip (no migration).
                return new MigrationResult(true, false, false, null);
            }
            legacy = obj;
        }
        catch (Exception ex)
        {
            log.LogWarning(ex, "[LegacyProfileMigrator] failed to read {Path}", legacyFilePath);
            return new MigrationResult(true, false, false, ex.Message);
        }

        var cache = settings.Get<JsonObject>(CharacterProfileStore.GameCacheKey) ?? new JsonObject();
        var changed = false;

        // Identity fields — only fill empty slots.
        changed |= FillIdentityString(cache, "player_name", ReadString(legacy, "username"));
        changed |= FillIdentityString(cache, "profession_name", ReadString(legacy, "profession"));
        changed |= FillIdentityString(cache, "player_id", ReadString(legacy, "uid"));
        if (TryReadInt(legacy, "level", out var lv) && lv > 0)
        {
            if (!HasPositiveInt(cache, "level_base"))
            {
                cache["level_base"] = lv;
                changed = true;
            }
        }

        // Stats — only fill keys not already present in canonical store.
        var stats = settings.Get<JsonObject>(CharacterProfileStore.PlayerStatsKey) ?? new JsonObject();
        var statsChanged = false;
        foreach (var key in StatsFields)
        {
            if (legacy[key] is { } legacyValue && stats[key] is null)
            {
                stats[key] = legacyValue.DeepClone();
                statsChanged = true;
            }
        }

        if (!changed && !statsChanged)
        {
            // Legacy file existed but had nothing useful — still try to remove it
            // so a misformed file doesn't keep tripping the migration each launch.
            var deleted = TryDelete(legacyFilePath, log);
            return new MigrationResult(true, false, deleted, null);
        }

        if (changed)
        {
            settings.Set(CharacterProfileStore.GameCacheKey, cache);
        }
        if (statsChanged)
        {
            settings.Set(CharacterProfileStore.PlayerStatsKey, stats);
        }

        try
        {
            settings.Save();
        }
        catch (Exception ex)
        {
            log.LogWarning(ex, "[LegacyProfileMigrator] settings.Save failed; legacy file kept");
            return new MigrationResult(true, true, false, ex.Message);
        }

        var ok = TryDelete(legacyFilePath, log);
        return new MigrationResult(true, true, ok, null);
    }

    private static bool TryDelete(string path, ILogger log)
    {
        try
        {
            File.Delete(path);
            return true;
        }
        catch (Exception ex)
        {
            log.LogWarning(ex, "[LegacyProfileMigrator] failed to delete {Path}", path);
            return false;
        }
    }

    private static string ReadString(JsonObject obj, string key)
    {
        if (obj[key] is not JsonValue jv) return string.Empty;
        if (jv.TryGetValue<string>(out var s)) return s.Trim();
        return string.Empty;
    }

    private static bool TryReadInt(JsonObject obj, string key, out int value)
    {
        value = 0;
        if (obj[key] is not JsonValue jv) return false;
        if (jv.TryGetValue<int>(out value)) return true;
        if (jv.TryGetValue<long>(out var l)) { value = (int)l; return true; }
        if (jv.TryGetValue<double>(out var d)) { value = (int)d; return true; }
        if (jv.TryGetValue<string>(out var s) && int.TryParse(s, out value)) return true;
        return false;
    }

    private static bool FillIdentityString(JsonObject cache, string key, string legacyValue)
    {
        if (string.IsNullOrEmpty(legacyValue)) return false;
        var existing = cache[key]?.GetValue<string>();
        if (!string.IsNullOrEmpty(existing)) return false;
        cache[key] = legacyValue;
        return true;
    }

    private static bool HasPositiveInt(JsonObject cache, string key)
    {
        if (cache[key] is not JsonValue jv) return false;
        if (jv.TryGetValue<int>(out var i)) return i > 0;
        if (jv.TryGetValue<long>(out var l)) return l > 0;
        if (jv.TryGetValue<double>(out var d)) return d > 0;
        return false;
    }
}
