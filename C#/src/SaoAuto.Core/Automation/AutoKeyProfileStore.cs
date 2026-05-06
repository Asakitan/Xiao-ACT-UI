using System.Collections.Immutable;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Unicode;

namespace SaoAuto.Core.Automation;

/// <summary>
/// S74 — auto_key profile CRUD + JSON I/O. C# port of
/// <c>auto_key_engine.py</c> lines 339–540
/// (`normalize_auto_key_config`, `find_profile`, `upsert_profile`,
/// `delete_profile`, `clone_profile`, `summarize_profile`,
/// `export_profile_json`, `export_profile_to_default_path`,
/// `import_profile_from_path`).
///
/// Pure functions over immutable records — every mutator returns a
/// new <see cref="AutoKeyConfig"/>; the in-place dict mutation in
/// Python is replaced with `with` expressions. Disk I/O is its own
/// pair of methods (Export/Import) so callers can compose them with
/// any settings store.
/// </summary>
public static class AutoKeyProfileStore
{
    private static readonly JsonSerializerOptions ExportJsonOptions = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.Create(UnicodeRanges.All),
    };

    public static AutoKeyConfig DefaultConfig() => new(
        Enabled: false,
        ActiveProfileId: "",
        ServerUrl: AutoKeyProfileSpec.DefaultServerUrl,
        Profiles: ImmutableArray<AutoKeyProfileSpecRecord>.Empty);

    public static AutoKeyConfig NormalizeConfig(
        JsonElement raw,
        AuthorSnapshot? authorFallback = null,
        Func<string>? newId = null,
        Func<DateTimeOffset>? clock = null)
    {
        var d = DefaultConfig();
        var enabled = raw.ValueKind == JsonValueKind.Object && raw.TryGetProperty("enabled", out var e) && e.ValueKind == JsonValueKind.True;
        var serverUrl = ReadStringTrim(raw, "server_url");
        if (serverUrl.Length == 0) serverUrl = AutoKeyProfileSpec.DefaultServerUrl;
        var activeId = ReadStringTrim(raw, "active_profile_id");

        var profiles = ImmutableArray.CreateBuilder<AutoKeyProfileSpecRecord>();
        if (raw.ValueKind == JsonValueKind.Object && raw.TryGetProperty("profiles", out var pArr) && pArr.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in pArr.EnumerateArray())
                profiles.Add(AutoKeyProfileSpec.NormalizeProfile(item, authorFallback, newId: newId, clock: clock));
        }
        var built = profiles.ToImmutable();

        if (activeId.Length > 0 && !built.Any(p => p.Id == activeId)) activeId = "";
        if (activeId.Length == 0 && built.Length > 0) activeId = built[0].Id;

        return d with { Enabled = enabled, ActiveProfileId = activeId, ServerUrl = serverUrl, Profiles = built };
    }

    public static AutoKeyProfileSpecRecord? FindProfile(AutoKeyConfig config, string profileId)
    {
        if (string.IsNullOrEmpty(profileId)) return null;
        foreach (var p in config.Profiles) if (p.Id == profileId) return p;
        return null;
    }

    public static AutoKeyProfileSpecRecord? ActiveProfile(AutoKeyConfig config)
        => FindProfile(config, config.ActiveProfileId);

    public static AutoKeyConfig UpsertProfile(AutoKeyConfig config, AutoKeyProfileSpecRecord profile, bool activate = false)
    {
        var list = config.Profiles.ToBuilder();
        var replaced = false;
        for (var i = 0; i < list.Count; i++)
        {
            if (list[i].Id == profile.Id)
            {
                list[i] = profile;
                replaced = true;
                break;
            }
        }
        if (!replaced) list.Add(profile);
        var newActive = activate || string.IsNullOrEmpty(config.ActiveProfileId)
            ? profile.Id
            : config.ActiveProfileId;
        return config with { Profiles = list.ToImmutable(), ActiveProfileId = newActive };
    }

    public static AutoKeyConfig DeleteProfile(AutoKeyConfig config, string profileId)
    {
        if (string.IsNullOrEmpty(profileId)) return config;
        var kept = config.Profiles.Where(p => p.Id != profileId).ToImmutableArray();
        var newActive = config.ActiveProfileId == profileId
            ? (kept.Length > 0 ? kept[0].Id : "")
            : config.ActiveProfileId;
        return config with { Profiles = kept, ActiveProfileId = newActive };
    }

    /// <summary>
    /// Deep-copy + new id + " Copy" suffix + reset action ids + reset
    /// source/remote/timestamps. Returns the cloned profile and the
    /// new config (or null if the source id was not found).
    /// </summary>
    public static (AutoKeyConfig Config, AutoKeyProfileSpecRecord? Cloned) CloneProfile(
        AutoKeyConfig config,
        string profileId,
        AuthorSnapshot? authorOverride = null,
        Func<string>? newId = null,
        Func<DateTimeOffset>? clock = null)
    {
        var src = FindProfile(config, profileId);
        if (src is null) return (config, null);

        var nowIso = AutoKeyProfileSpec.UtcNowIso(clock);
        var clonedActions = src.Actions
            .Select(a => a with { Id = AutoKeyProfileSpec.NewId("action", newId) })
            .ToImmutableArray();
        var name = string.IsNullOrEmpty(src.ProfileName) ? "Profile" : src.ProfileName;
        var author = authorOverride ?? src.AuthorSnapshot;
        var cloned = src with
        {
            Id = AutoKeyProfileSpec.NewId("profile", newId),
            ProfileName = $"{name} Copy",
            Source = "local",
            RemoteId = null,
            CreatedAt = nowIso,
            UpdatedAt = nowIso,
            AuthorSnapshot = author,
            Actions = clonedActions,
        };
        var nextConfig = UpsertProfile(config, cloned, activate: false);
        return (nextConfig, cloned);
    }

    public static AutoKeyProfileSummary SummarizeProfile(AutoKeyProfileSpecRecord profile)
    {
        var enabled = profile.Actions.Count(a => a.Enabled);
        return new AutoKeyProfileSummary(
            Id: profile.Id,
            ProfileName: profile.ProfileName,
            Description: profile.Description,
            ProfessionId: profile.ProfessionId,
            ProfessionName: profile.ProfessionName,
            Source: string.IsNullOrEmpty(profile.Source) ? "local" : profile.Source,
            RemoteId: profile.RemoteId,
            UpdatedAt: profile.UpdatedAt,
            ActionCount: profile.Actions.Length,
            EnabledActionCount: enabled);
    }

    /// <summary>
    /// Wraps the profile in <c>{"schema_version", "profile"}</c> and
    /// pretty-prints. Matches Python's <c>export_profile_json</c>.
    /// </summary>
    public static string ExportProfileJson(AutoKeyProfileSpecRecord profile)
    {
        var dto = new ExportEnvelope(AutoKeyProfileSpec.SchemaVersion, ProfileToDto(profile));
        return JsonSerializer.Serialize(dto, ExportJsonOptions);
    }

    public static string EnsureExportDir(string baseDir)
    {
        Directory.CreateDirectory(baseDir);
        return baseDir;
    }

    public static string ExportProfileToPath(
        AutoKeyProfileSpecRecord profile,
        string exportDir,
        Func<DateTimeOffset>? localClock = null)
    {
        EnsureExportDir(exportDir);
        var stamp = (localClock ?? (() => DateTimeOffset.Now))().LocalDateTime
            .ToString("yyyyMMdd_HHmmss", System.Globalization.CultureInfo.InvariantCulture);
        var name = $"{AutoKeyProfileSpec.Slugify(profile.ProfileName)}_{stamp}.json";
        var path = Path.Combine(exportDir, name);
        File.WriteAllText(path, ExportProfileJson(profile), System.Text.Encoding.UTF8);
        return path;
    }

    public static AutoKeyProfileSpecRecord ImportProfileFromPath(
        string path,
        AuthorSnapshot? authorFallback = null,
        Func<string>? newId = null,
        Func<DateTimeOffset>? clock = null)
    {
        using var fs = File.OpenRead(path);
        using var doc = JsonDocument.Parse(fs);
        return ImportProfile(doc.RootElement, authorFallback, newId, clock);
    }

    public static AutoKeyProfileSpecRecord ImportProfile(
        JsonElement raw,
        AuthorSnapshot? authorFallback = null,
        Func<string>? newId = null,
        Func<DateTimeOffset>? clock = null)
    {
        JsonElement profileNode = raw;
        if (raw.ValueKind == JsonValueKind.Object && raw.TryGetProperty("profile", out var inner)
            && inner.ValueKind == JsonValueKind.Object)
        {
            profileNode = inner;
        }
        var normalized = AutoKeyProfileSpec.NormalizeProfile(profileNode, authorFallback, sourceOverride: "local", newId: newId, clock: clock);
        var nowIso = AutoKeyProfileSpec.UtcNowIso(clock);
        var freshActions = normalized.Actions
            .Select(a => a with { Id = AutoKeyProfileSpec.NewId("action", newId) })
            .ToImmutableArray();
        return normalized with
        {
            Id = AutoKeyProfileSpec.NewId("profile", newId),
            RemoteId = null,
            Source = "local",
            CreatedAt = nowIso,
            UpdatedAt = nowIso,
            Actions = freshActions,
        };
    }

    // ── helpers ─────────────────────────────────────────────────

    private static string ReadStringTrim(JsonElement obj, string key)
    {
        if (obj.ValueKind != JsonValueKind.Object) return string.Empty;
        return obj.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.String
            ? (v.GetString() ?? string.Empty).Trim()
            : string.Empty;
    }

    private static ProfileDto ProfileToDto(AutoKeyProfileSpecRecord p) => new(
        id: p.Id,
        schema_version: p.SchemaVersion,
        profile_name: p.ProfileName,
        description: p.Description,
        profession_id: p.ProfessionId,
        profession_name: p.ProfessionName,
        source: p.Source,
        remote_id: p.RemoteId,
        created_at: p.CreatedAt,
        updated_at: p.UpdatedAt,
        author_snapshot: new AuthorDto(p.AuthorSnapshot.PlayerUid, p.AuthorSnapshot.PlayerName, p.AuthorSnapshot.ProfessionId, p.AuthorSnapshot.ProfessionName),
        engine: new EngineDto(p.Engine.TickMs, p.Engine.RequireForeground, p.Engine.PauseOnDeath),
        actions: p.Actions.Select(ActionToDto).ToArray());

    private static ActionDto ActionToDto(AutoKeyActionSpec a) => new(
        id: a.Id, label: a.Label, enabled: a.Enabled, slot_index: a.SlotIndex,
        key: a.Key, press_mode: a.PressMode, press_count: a.PressCount,
        press_interval_ms: a.PressIntervalMs, hold_ms: a.HoldMs,
        ready_delay_ms: a.ReadyDelayMs, min_rearm_ms: a.MinRearmMs,
        post_delay_ms: a.PostDelayMs,
        conditions: a.Conditions.Select(ConditionToDto).ToArray());

    private static Dictionary<string, object?> ConditionToDto(AutoKeyCondition c) => c switch
    {
        HpPctGteCondition x => new() { ["type"] = "hp_pct_gte", ["value"] = x.Value },
        HpPctLteCondition x => new() { ["type"] = "hp_pct_lte", ["value"] = x.Value },
        StaPctGteCondition x => new() { ["type"] = "sta_pct_gte", ["value"] = x.Value },
        BurstReadyIsCondition x => new() { ["type"] = "burst_ready_is", ["value"] = x.Value },
        SlotStateIsCondition x => new() { ["type"] = "slot_state_is", ["slot_index"] = x.SlotIndex, ["state"] = x.State },
        ProfessionIsCondition x => new() { ["type"] = "profession_is", ["value"] = x.Value },
        PlayerNameIsCondition x => new() { ["type"] = "player_name_is", ["value"] = x.Value },
        InCombatIsCondition x => new() { ["type"] = "in_combat_is", ["value"] = x.Value },
        _ => new(),
    };

    // Pyhon-snake-case DTOs only used to pin export JSON shape.
    private record ExportEnvelope(int schema_version, ProfileDto profile);
    private record ProfileDto(
        string id, int schema_version, string profile_name, string description,
        int profession_id, string profession_name, string source, string? remote_id,
        string created_at, string updated_at,
        AuthorDto author_snapshot, EngineDto engine, ActionDto[] actions);
    private record AuthorDto(string player_uid, string player_name, int profession_id, string profession_name);
    private record EngineDto(int tick_ms, bool require_foreground, bool pause_on_death);
    private record ActionDto(
        string id, string label, bool enabled, int slot_index, string key,
        string press_mode, int press_count, int press_interval_ms, int hold_ms,
        int ready_delay_ms, int min_rearm_ms, int post_delay_ms,
        Dictionary<string, object?>[] conditions);
}

public sealed record AutoKeyConfig(
    bool Enabled,
    string ActiveProfileId,
    string ServerUrl,
    ImmutableArray<AutoKeyProfileSpecRecord> Profiles);

public sealed record AutoKeyProfileSummary(
    string Id,
    string ProfileName,
    string Description,
    int ProfessionId,
    string ProfessionName,
    string Source,
    string? RemoteId,
    string UpdatedAt,
    int ActionCount,
    int EnabledActionCount);
