using System.Globalization;
using System.Text.Json;

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
