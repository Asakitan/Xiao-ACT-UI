using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Core.Automation;

/// <summary>
/// Filesystem and config-shape adapters for boss-raid profiles —
/// port of <c>boss_raid_engine.{ensure_export_dir, export_profile_to_default_path,
/// import_profile_from_path, build_boss_raid_state, save_boss_raid_config,
/// load_boss_raid_config}</c>.
/// Pure CPU + file I/O. Caller passes the export directory (Python uses
/// a global <c>BOSS_RAID_EXPORT_DIR</c>); caller also injects a
/// <see cref="LocalNowFactory"/> for deterministic filename stamps in tests.
/// </summary>
public static class BossRaidProfileIo
{
    /// <summary>Inject for deterministic export filenames in tests.</summary>
    public static Func<DateTime> LocalNowFactory { get; set; } = () => DateTime.Now;

    public static string EnsureExportDir(string baseDir)
    {
        Directory.CreateDirectory(baseDir);
        return baseDir;
    }

    /// <summary>Write the profile JSON to
    /// <c>{baseDir}/{slug}_{YYYYMMDD_HHMMSS}.json</c> and return the path.</summary>
    public static string ExportProfileToDefaultPath(RaidProfile profile, string baseDir)
    {
        var dir = EnsureExportDir(baseDir);
        var stamp = LocalNowFactory().ToString("yyyyMMdd_HHmmss", CultureInfo.InvariantCulture);
        var slug = BossRaidProfile.SlugifyFilename(profile.ProfileName);
        var path = Path.Combine(dir, $"{slug}_{stamp}.json");
        File.WriteAllText(path, BossRaidProfile.ExportProfileJson(profile), System.Text.Encoding.UTF8);
        return path;
    }

    /// <summary>Read JSON from <paramref name="path"/>, normalize as a profile,
    /// and regenerate id/phase/timeline IDs (force-local source). Mirrors
    /// Python <c>import_profile_from_path</c>.</summary>
    public static RaidProfile ImportProfileFromPath(string path, JsonElement? authorSnapshot = null)
    {
        var text = File.ReadAllText(path, System.Text.Encoding.UTF8);
        using var doc = JsonDocument.Parse(text);
        var root = doc.RootElement.Clone();

        // Python: data.get("profile") if isinstance(data, dict) else {}
        //         if not profile_data and isinstance(data, dict): profile_data = data
        JsonElement profileEl;
        if (root.ValueKind == JsonValueKind.Object && root.TryGetProperty("profile", out var inner)
            && inner.ValueKind == JsonValueKind.Object)
        {
            profileEl = inner;
        }
        else if (root.ValueKind == JsonValueKind.Object)
        {
            profileEl = root;
        }
        else
        {
            profileEl = JsonDocument.Parse("{}").RootElement;
        }

        var normalized = BossRaidProfile.NormalizeProfile(profileEl, authorSnapshot, source: "local");
        var nowFactory = BossRaidProfile.UtcNowIsoFactory;
        var idFactory = BossRaidProfile.NewIdFactory;
        var now = nowFactory();

        var newPhases = new List<RaidProfilePhase>(normalized.Phases.Count);
        foreach (var ph in normalized.Phases)
        {
            var newTimelines = ph.Timelines
                .Select(tl => tl with { Id = idFactory("tl") })
                .ToList();
            newPhases.Add(ph with { Id = idFactory("phase"), Timelines = newTimelines });
        }
        return normalized with
        {
            Id = idFactory("boss"),
            RemoteId = null,
            Source = "local",
            CreatedAt = now,
            UpdatedAt = now,
            Phases = newPhases,
        };
    }

    /// <summary>Build the dictionary-shaped state payload consumed by the
    /// WPF / WebView panel. Mirrors Python <c>build_boss_raid_state</c>.</summary>
    public static BossRaidStateView BuildBossRaidState(
        RaidConfig config,
        JsonElement? engineStatus = null,
        JsonElement? uploadAuth = null)
    {
        var ap = BossRaidProfile.ActiveProfile(config);
        return new BossRaidStateView(
            Enabled: config.Enabled,
            ActiveProfileId: config.ActiveProfileId,
            ActiveProfileName: ap?.ProfileName ?? string.Empty,
            Profiles: config.Profiles.Select(BossRaidProfile.SummarizeProfile).ToArray(),
            ProfilesFull: config.Profiles,
            ActiveProfile: ap,
            LocalProfileCount: config.Profiles.Count,
            ServerUrl: string.IsNullOrEmpty(config.ServerUrl) ? BossRaidProfile.DefaultServerUrl : config.ServerUrl,
            UploadAuth: uploadAuth,
            LastRemoteSearch: config.LastRemoteSearch,
            Runtime: engineStatus);
    }
}

/// <summary>S175 — settings adapters mirroring Python
/// <c>load_boss_raid_config</c> / <c>save_boss_raid_config</c>. Reads
/// the <c>boss_raid</c> key from <see cref="SettingsManager"/>, runs it
/// through <see cref="BossRaidProfile.NormalizeConfig"/>, and (on save)
/// persists the normalized payload back with Python-compatible snake-case
/// keys (notably <c>time_s</c>, which doesn't round-trip through any
/// stock naming policy).</summary>
public static class BossRaidConfigStore
{
    public const string SettingsKey = "boss_raid";

    public static RaidConfig Load(SettingsManager settings, JsonElement? stateSnapshot = null)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        var node = settings.Get<JsonNode>(SettingsKey);
        if (node is null)
        {
            using var empty = JsonDocument.Parse("{}");
            return BossRaidProfile.NormalizeConfig(empty.RootElement.Clone(), stateSnapshot);
        }
        using var doc = JsonDocument.Parse(node.ToJsonString());
        return BossRaidProfile.NormalizeConfig(doc.RootElement.Clone(), stateSnapshot);
    }

    public static RaidConfig Save(SettingsManager settings, RaidConfig config)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        if (config is null) throw new ArgumentNullException(nameof(config));
        // Python re-normalizes on save; we do the same so corruption from
        // a misbehaving caller never reaches disk.
        var preNode = ConfigToJsonObject(config);
        using (var preDoc = JsonDocument.Parse(preNode.ToJsonString()))
        {
            config = BossRaidProfile.NormalizeConfig(preDoc.RootElement.Clone());
        }
        settings.Set<JsonNode>(SettingsKey, ConfigToJsonObject(config));
        settings.Save();
        return config;
    }

    // ── Hand-mapped snake_case serializer (avoids the time_s/TimeSeconds drift
    //    that any stock naming policy would introduce). ───────────────────────

    public static JsonObject ConfigToJsonObject(RaidConfig c) => new()
    {
        ["enabled"] = c.Enabled,
        ["active_profile_id"] = c.ActiveProfileId,
        ["server_url"] = c.ServerUrl,
        ["profiles"] = ProfilesArray(c.Profiles),
        ["last_remote_search"] = SearchObject(c.LastRemoteSearch),
    };

    private static JsonArray ProfilesArray(IReadOnlyList<RaidProfile> profiles)
    {
        var arr = new JsonArray();
        foreach (var p in profiles) arr.Add(ProfileObject(p));
        return arr;
    }

    private static JsonObject ProfileObject(RaidProfile p) => new()
    {
        ["id"] = p.Id,
        ["schema_version"] = p.SchemaVersion,
        ["profile_name"] = p.ProfileName,
        ["description"] = p.Description,
        ["boss_total_hp"] = p.BossTotalHp,
        ["enrage_time_s"] = p.EnrageTimeS,
        ["simple_mode"] = p.SimpleMode,
        ["target_name_pattern"] = p.TargetNamePattern,
        ["phases"] = PhasesArray(p.Phases),
        ["source"] = p.Source,
        ["remote_id"] = p.RemoteId is null ? null : JsonValue.Create(p.RemoteId),
        ["created_at"] = p.CreatedAt,
        ["updated_at"] = p.UpdatedAt,
        ["author_snapshot"] = AuthorObject(p.AuthorSnapshot),
    };

    private static JsonArray PhasesArray(IReadOnlyList<RaidProfilePhase> phases)
    {
        var arr = new JsonArray();
        foreach (var ph in phases) arr.Add(PhaseObject(ph));
        return arr;
    }

    private static JsonObject PhaseObject(RaidProfilePhase ph) => new()
    {
        ["id"] = ph.Id,
        ["name"] = ph.Name,
        ["trigger"] = new JsonObject { ["type"] = ph.Trigger.Type, ["value"] = ph.Trigger.Value },
        ["timelines"] = TimelinesArray(ph.Timelines),
    };

    private static JsonArray TimelinesArray(IReadOnlyList<RaidTimelineEntry> timelines)
    {
        var arr = new JsonArray();
        foreach (var tl in timelines) arr.Add(TimelineObject(tl));
        return arr;
    }

    private static JsonObject TimelineObject(RaidTimelineEntry tl) => new()
    {
        ["id"] = tl.Id,
        ["time_s"] = tl.TimeSeconds,
        ["label"] = tl.Label,
        ["alert_type"] = tl.AlertType,
        ["repeat_interval_s"] = tl.RepeatIntervalS,
        ["pre_warn_s"] = tl.PreWarnS,
        ["duration_s"] = tl.DurationS,
        ["condition"] = tl.Condition is null
            ? null
            : new JsonObject
            {
                ["type"] = tl.Condition.Type,
                ["comparator"] = tl.Condition.Comparator,
                ["value"] = tl.Condition.Value,
            },
    };

    private static JsonObject AuthorObject(RaidAuthor a) => new()
    {
        ["player_uid"] = a.PlayerUid,
        ["player_name"] = a.PlayerName,
        ["profession_id"] = a.ProfessionId,
        ["profession_name"] = a.ProfessionName,
    };

    private static JsonObject SearchObject(RaidRemoteSearch s)
    {
        var results = new JsonArray();
        foreach (var r in s.Results) results.Add(JsonNode.Parse(r.GetRawText()));
        return new JsonObject
        {
            ["query"] = new JsonObject
            {
                ["q"] = s.Query.Q,
                ["page"] = s.Query.Page,
                ["page_size"] = s.Query.PageSize,
            },
            ["results"] = results,
            ["error"] = s.Error,
            ["fetched_at"] = s.FetchedAt,
        };
    }
}

public sealed record BossRaidStateView(
    bool Enabled,
    string ActiveProfileId,
    string ActiveProfileName,
    IReadOnlyList<RaidProfileSummary> Profiles,
    IReadOnlyList<RaidProfile> ProfilesFull,
    RaidProfile? ActiveProfile,
    int LocalProfileCount,
    string ServerUrl,
    JsonElement? UploadAuth,
    RaidRemoteSearch LastRemoteSearch,
    JsonElement? Runtime);
