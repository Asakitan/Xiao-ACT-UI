using System.Globalization;
using System.Text;
using System.Text.Json;

namespace SaoAuto.Core.Automation;

/// <summary>
/// Pure-CPU port of the profile / timeline / phase normalization +
/// config storage helpers from <c>boss_raid_engine.py</c> (lines 27–417).
/// HTTP cloud client, settings adapter, and filesystem export wrapper
/// remain on the Python side.
///
/// The Python helpers take <c>Any</c> input and produce dict output;
/// the C# port takes <see cref="JsonElement"/> input (deserialized
/// payload) and produces strongly-typed records. Defaults match
/// Python bit-exact; coercion clamps mirror Python's <c>min/max</c>.
/// </summary>
public static class BossRaidProfile
{
    public const int SchemaVersion = 1;
    public const string DefaultServerUrl = "http://47.82.157.220:9320";

    private static readonly string[] AllowedConditionTypes =
        { "hp_pct", "shield_active", "breaking", "always" };

    private static readonly string[] AllowedTriggerTypes =
    {
        "manual", "time", "dps_total", "hp_pct", "breaking", "buff_event",
        "shield_broken", "overdrive", "extinction_pct", "breaking_stage",
    };

    private static readonly string[] AllowedSources = { "local", "downloaded", "uploaded" };

    /// <summary>Inject for deterministic IDs in tests; default is a UUID-ish hex.</summary>
    public static Func<string, string> NewIdFactory { get; set; } =
        prefix => $"{prefix}_{Guid.NewGuid().ToString("N")[..12]}";

    /// <summary>Inject for deterministic timestamps in tests; default is current UTC.</summary>
    public static Func<string> UtcNowIsoFactory { get; set; } =
        () => DateTimeOffset.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture);

    // ── Coerce helpers (Python `_coerce_*`) ─────────────────────────────

    public static bool CoerceBool(JsonElement el, bool @default = false)
    {
        switch (el.ValueKind)
        {
            case JsonValueKind.True: return true;
            case JsonValueKind.False: return false;
            case JsonValueKind.String:
                var t = (el.GetString() ?? string.Empty).Trim().ToLowerInvariant();
                if (t is "1" or "true" or "yes" or "on") return true;
                if (t is "0" or "false" or "no" or "off") return false;
                return @default;
            case JsonValueKind.Number:
                return el.TryGetDouble(out var d) && d != 0.0;
            default:
                return @default;
        }
    }

    public static int CoerceInt(JsonElement el, int @default = 0, int? minimum = null, int? maximum = null)
    {
        int result;
        try
        {
            result = el.ValueKind switch
            {
                JsonValueKind.Number => el.TryGetInt32(out var i) ? i : (int)el.GetDouble(),
                JsonValueKind.String => int.TryParse(el.GetString(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var s)
                    ? s
                    : (int)double.Parse(el.GetString()!, CultureInfo.InvariantCulture),
                JsonValueKind.True => 1,
                JsonValueKind.False => 0,
                _ => @default,
            };
        }
        catch
        {
            result = @default;
        }
        if (minimum.HasValue) result = Math.Max(minimum.Value, result);
        if (maximum.HasValue) result = Math.Min(maximum.Value, result);
        return result;
    }

    public static double CoerceFloat(JsonElement el, double @default = 0.0, double? minimum = null, double? maximum = null)
    {
        double result;
        try
        {
            result = el.ValueKind switch
            {
                JsonValueKind.Number => el.GetDouble(),
                JsonValueKind.String => double.Parse(el.GetString()!, NumberStyles.Float, CultureInfo.InvariantCulture),
                JsonValueKind.True => 1.0,
                JsonValueKind.False => 0.0,
                _ => @default,
            };
        }
        catch
        {
            result = @default;
        }
        if (minimum.HasValue) result = Math.Max(minimum.Value, result);
        if (maximum.HasValue) result = Math.Min(maximum.Value, result);
        return result;
    }

    public static string CoerceString(JsonElement el)
    {
        if (el.ValueKind == JsonValueKind.Null || el.ValueKind == JsonValueKind.Undefined) return string.Empty;
        if (el.ValueKind == JsonValueKind.String) return (el.GetString() ?? string.Empty).Trim();
        if (el.ValueKind == JsonValueKind.Number) return el.GetRawText();
        if (el.ValueKind == JsonValueKind.True) return "True";
        if (el.ValueKind == JsonValueKind.False) return "False";
        return string.Empty;
    }

    /// <summary>Filename-safe slug — alphanumeric + underscore + dash + CJK
    /// kept; everything else mapped to <c>_</c>; trims surrounding underscores;
    /// falls back to <c>boss_raid_profile</c>.</summary>
    public static string SlugifyFilename(string? text)
    {
        var value = (text ?? "boss_raid_profile").Trim().Replace(" ", "_");
        var sb = new StringBuilder(value.Length);
        foreach (var ch in value)
        {
            if (char.IsLetterOrDigit(ch) || ch is '-' or '_') sb.Append(ch);
            else if (ch >= '\u4e00' && ch <= '\u9fff') sb.Append(ch);
            else sb.Append('_');
        }
        var cleaned = sb.ToString().Trim('_');
        return cleaned.Length == 0 ? "boss_raid_profile" : cleaned;
    }

    /// <summary>Mask the middle of a token — Python <c>_mask_token</c>.
    /// Empty input → empty; ≤8 chars → all stars; otherwise first 4 + "..." + last 4.</summary>
    public static string MaskToken(string? token)
    {
        var t = (token ?? string.Empty).Trim();
        if (t.Length == 0) return string.Empty;
        if (t.Length <= 8) return new string('*', t.Length);
        return $"{t[..4]}...{t[^4..]}";
    }

    // ── Defaults ────────────────────────────────────────────────────────

    public static RaidTimelineEntry MakeDefaultTimeline() => new(
        Id: NewIdFactory("tl"),
        TimeSeconds: 30.0,
        Label: "Alert",
        AlertType: "both",
        RepeatIntervalS: 0.0,
        PreWarnS: 0.0,
        DurationS: 0.0,
        Condition: null);

    public static RaidPhaseTrigger MakeDefaultPhaseTrigger() => new(Type: "manual", Value: 0.0);

    public static RaidProfilePhase MakeDefaultPhase(int index = 1) => new(
        Id: NewIdFactory("phase"),
        Name: $"P{index}",
        Trigger: MakeDefaultPhaseTrigger(),
        Timelines: Array.Empty<RaidTimelineEntry>());

    public static RaidAuthor MakeDefaultAuthor() => new(
        PlayerUid: string.Empty,
        PlayerName: string.Empty,
        ProfessionId: 0,
        ProfessionName: string.Empty);

    public static RaidProfile MakeDefaultProfile(JsonElement? authorSnapshot = null)
    {
        var now = UtcNowIsoFactory();
        return new RaidProfile(
            Id: NewIdFactory("boss"),
            SchemaVersion: SchemaVersion,
            ProfileName: "New Boss Raid",
            Description: string.Empty,
            BossTotalHp: 0,
            EnrageTimeS: 600,
            SimpleMode: true,
            TargetNamePattern: string.Empty,
            Phases: new[] { MakeDefaultPhase(1) },
            Source: "local",
            RemoteId: null,
            CreatedAt: now,
            UpdatedAt: now,
            AuthorSnapshot: NormalizeAuthor(authorSnapshot));
    }

    public static RaidConfig DefaultConfig() => new(
        Enabled: false,
        ActiveProfileId: string.Empty,
        ServerUrl: DefaultServerUrl,
        Profiles: Array.Empty<RaidProfile>(),
        LastRemoteSearch: new RaidRemoteSearch(
            Query: new RaidRemoteQuery(Q: string.Empty, Page: 1, PageSize: 20),
            Results: Array.Empty<JsonElement>(),
            Error: string.Empty,
            FetchedAt: string.Empty));

    // ── Normalize* (Python parity) ──────────────────────────────────────

    public static RaidTimelineCondition? NormalizeTimelineCondition(JsonElement? raw)
    {
        if (raw is null || raw.Value.ValueKind != JsonValueKind.Object) return null;
        var src = raw.Value;
        var rawType = CoerceString(src.GetOrUndefined("type"));
        var type = AllowedConditionTypes.Contains(rawType) ? rawType : "always";
        var comparator = CoerceString(src.GetOrUndefined("comparator"));
        if (string.IsNullOrEmpty(comparator)) comparator = ">=";
        return new RaidTimelineCondition(
            Type: type,
            Comparator: comparator,
            Value: CoerceFloat(src.GetOrUndefined("value"), 0.0));
    }

    public static RaidTimelineEntry NormalizeTimeline(JsonElement raw)
    {
        if (raw.ValueKind != JsonValueKind.Object) raw = EmptyObject;
        var idStr = CoerceString(raw.GetOrUndefined("id"));
        return new RaidTimelineEntry(
            Id: string.IsNullOrEmpty(idStr) ? NewIdFactory("tl") : idStr,
            TimeSeconds: CoerceFloat(raw.GetOrUndefined("time_s"), 30.0, 0.0, 86400.0),
            Label: NonEmpty(CoerceString(raw.GetOrUndefined("label")), "Alert"),
            AlertType: NonEmpty(CoerceString(raw.GetOrUndefined("alert_type")), "both"),
            RepeatIntervalS: CoerceFloat(raw.GetOrUndefined("repeat_interval_s"), 0.0, 0.0, 86400.0),
            PreWarnS: CoerceFloat(raw.GetOrUndefined("pre_warn_s"), 0.0, 0.0, 600.0),
            DurationS: CoerceFloat(raw.GetOrUndefined("duration_s"), 0.0, 0.0, 600.0),
            Condition: NormalizeTimelineCondition(raw.GetOrUndefined("condition") is { ValueKind: JsonValueKind.Object } c ? c : (JsonElement?)null));
    }

    public static RaidPhaseTrigger NormalizePhaseTrigger(JsonElement raw)
    {
        if (raw.ValueKind != JsonValueKind.Object) raw = EmptyObject;
        var t = CoerceString(raw.GetOrUndefined("type")).ToLowerInvariant();
        if (!AllowedTriggerTypes.Contains(t)) t = "manual";
        return new RaidPhaseTrigger(
            Type: t,
            Value: CoerceFloat(raw.GetOrUndefined("value"), 0.0, 0.0));
    }

    public static RaidProfilePhase NormalizePhase(JsonElement raw, int fallbackIndex = 1)
    {
        if (raw.ValueKind != JsonValueKind.Object) raw = EmptyObject;
        var timelines = new List<RaidTimelineEntry>();
        var tlsEl = raw.GetOrUndefined("timelines");
        if (tlsEl.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in tlsEl.EnumerateArray())
                timelines.Add(NormalizeTimeline(item));
        }
        var idStr = CoerceString(raw.GetOrUndefined("id"));
        var nameStr = CoerceString(raw.GetOrUndefined("name"));
        return new RaidProfilePhase(
            Id: string.IsNullOrEmpty(idStr) ? NewIdFactory("phase") : idStr,
            Name: NonEmpty(nameStr, $"P{fallbackIndex}"),
            Trigger: NormalizePhaseTrigger(raw.GetOrUndefined("trigger")),
            Timelines: timelines);
    }

    public static RaidAuthor NormalizeAuthor(JsonElement? raw)
    {
        if (raw is null || raw.Value.ValueKind != JsonValueKind.Object) return MakeDefaultAuthor();
        var src = raw.Value;
        var uid = CoerceString(src.GetOrUndefined("player_uid"));
        if (string.IsNullOrEmpty(uid)) uid = CoerceString(src.GetOrUndefined("uid"));
        var name = CoerceString(src.GetOrUndefined("player_name"));
        if (string.IsNullOrEmpty(name)) name = CoerceString(src.GetOrUndefined("name"));
        var profName = CoerceString(src.GetOrUndefined("profession_name"));
        if (string.IsNullOrEmpty(profName)) profName = CoerceString(src.GetOrUndefined("profession"));
        return new RaidAuthor(
            PlayerUid: uid,
            PlayerName: name,
            ProfessionId: CoerceInt(src.GetOrUndefined("profession_id"), 0, 0),
            ProfessionName: profName);
    }

    public static RaidProfile NormalizeProfile(JsonElement raw, JsonElement? authorSnapshot = null, string? source = null)
    {
        var @base = raw.ValueKind == JsonValueKind.Object ? raw : EmptyObject;
        var def = MakeDefaultProfile(authorSnapshot);
        var profileSource = (source ?? CoerceString(@base.GetOrUndefined("source"))).ToLowerInvariant();
        if (string.IsNullOrEmpty(profileSource)) profileSource = "local";
        if (!AllowedSources.Contains(profileSource)) profileSource = "local";

        // Python: `_normalize_author(base.get("author_snapshot") or author_snapshot)`.
        var authEl = @base.GetOrUndefined("author_snapshot");
        JsonElement? authIn = authEl.ValueKind == JsonValueKind.Object ? authEl : authorSnapshot;
        var author = NormalizeAuthor(authIn);

        var phases = new List<RaidProfilePhase>();
        var phasesEl = @base.GetOrUndefined("phases");
        if (phasesEl.ValueKind == JsonValueKind.Array)
        {
            int idx = 1;
            foreach (var item in phasesEl.EnumerateArray())
            {
                phases.Add(NormalizePhase(item, idx));
                idx++;
            }
        }
        if (phases.Count == 0) phases.Add(MakeDefaultPhase(1));

        var idStr = CoerceString(@base.GetOrUndefined("id"));
        var nameStr = CoerceString(@base.GetOrUndefined("profile_name"));
        var createdAt = CoerceString(@base.GetOrUndefined("created_at"));
        var remoteId = CoerceString(@base.GetOrUndefined("remote_id"));
        return new RaidProfile(
            Id: string.IsNullOrEmpty(idStr) ? def.Id : idStr,
            SchemaVersion: SchemaVersion,
            ProfileName: NonEmpty(nameStr, def.ProfileName),
            Description: CoerceString(@base.GetOrUndefined("description")),
            BossTotalHp: CoerceInt(@base.GetOrUndefined("boss_total_hp"), 0, 0),
            EnrageTimeS: CoerceInt(@base.GetOrUndefined("enrage_time_s"), 600, 0, 86400),
            SimpleMode: CoerceBool(@base.GetOrUndefined("simple_mode"), true),
            TargetNamePattern: CoerceString(@base.GetOrUndefined("target_name_pattern")),
            Phases: phases,
            Source: profileSource,
            RemoteId: string.IsNullOrEmpty(remoteId) ? null : remoteId,
            CreatedAt: string.IsNullOrEmpty(createdAt) ? def.CreatedAt : createdAt,
            UpdatedAt: UtcNowIsoFactory(),
            AuthorSnapshot: author);
    }

    public static RaidConfig NormalizeConfig(JsonElement raw, JsonElement? stateSnapshot = null)
    {
        var src = raw.ValueKind == JsonValueKind.Object ? raw : EmptyObject;
        var author = NormalizeAuthor(stateSnapshot);
        var enabled = CoerceBool(src.GetOrUndefined("enabled"), false);
        var activeId = CoerceString(src.GetOrUndefined("active_profile_id"));
        var serverUrl = CoerceString(src.GetOrUndefined("server_url"));
        if (string.IsNullOrEmpty(serverUrl)) serverUrl = DefaultServerUrl;

        var profiles = new List<RaidProfile>();
        var profilesEl = src.GetOrUndefined("profiles");
        if (profilesEl.ValueKind == JsonValueKind.Array)
        {
            JsonElement? authJson = author == MakeDefaultAuthor() ? null : (JsonElement?)WrapAuthor(author);
            foreach (var item in profilesEl.EnumerateArray())
                profiles.Add(NormalizeProfile(item, authorSnapshot: authJson));
        }

        // Drop active id if it doesn't match any profile; else default to first profile.
        if (!string.IsNullOrEmpty(activeId) && !profiles.Any(p => p.Id == activeId))
            activeId = string.Empty;
        if (string.IsNullOrEmpty(activeId) && profiles.Count > 0)
            activeId = profiles[0].Id;

        var searchEl = src.GetOrUndefined("last_remote_search");
        RaidRemoteSearch search;
        if (searchEl.ValueKind == JsonValueKind.Object)
        {
            var qEl = searchEl.GetOrUndefined("query");
            var qObj = qEl.ValueKind == JsonValueKind.Object ? qEl : EmptyObject;
            var resultsEl = searchEl.GetOrUndefined("results");
            var results = resultsEl.ValueKind == JsonValueKind.Array
                ? resultsEl.EnumerateArray().ToArray()
                : Array.Empty<JsonElement>();
            search = new RaidRemoteSearch(
                Query: new RaidRemoteQuery(
                    Q: CoerceString(qObj.GetOrUndefined("q")),
                    Page: CoerceInt(qObj.GetOrUndefined("page"), 1, 1),
                    PageSize: CoerceInt(qObj.GetOrUndefined("page_size"), 20, 1, 100)),
                Results: results,
                Error: CoerceString(searchEl.GetOrUndefined("error")),
                FetchedAt: CoerceString(searchEl.GetOrUndefined("fetched_at")));
        }
        else
        {
            search = DefaultConfig().LastRemoteSearch;
        }

        return new RaidConfig(
            Enabled: enabled,
            ActiveProfileId: activeId,
            ServerUrl: serverUrl,
            Profiles: profiles,
            LastRemoteSearch: search);
    }

    // ── Lookup / mutation ────────────────────────────────────────────────

    public static RaidProfile? FindProfile(RaidConfig config, string profileId)
    {
        var pid = (profileId ?? string.Empty).Trim();
        if (pid.Length == 0) return null;
        foreach (var p in config.Profiles)
            if (p.Id == pid) return p;
        return null;
    }

    public static RaidProfile? ActiveProfile(RaidConfig config) =>
        FindProfile(config, config.ActiveProfileId);

    public static RaidConfig UpsertProfile(RaidConfig config, RaidProfile profile, bool activate = false)
    {
        var list = new List<RaidProfile>(config.Profiles);
        bool replaced = false;
        for (int i = 0; i < list.Count; i++)
        {
            if (list[i].Id == profile.Id) { list[i] = profile; replaced = true; break; }
        }
        if (!replaced) list.Add(profile);
        var newActive = config.ActiveProfileId;
        if (activate || string.IsNullOrEmpty(newActive)) newActive = profile.Id;
        return config with { Profiles = list, ActiveProfileId = newActive };
    }

    public static RaidConfig DeleteProfile(RaidConfig config, string profileId)
    {
        var pid = (profileId ?? string.Empty).Trim();
        var list = config.Profiles.Where(p => p.Id != pid).ToList();
        var newActive = config.ActiveProfileId == pid
            ? (list.Count > 0 ? list[0].Id : string.Empty)
            : config.ActiveProfileId;
        return config with { Profiles = list, ActiveProfileId = newActive };
    }

    public static (RaidConfig Config, RaidProfile? Cloned) CloneProfile(
        RaidConfig config, string profileId, JsonElement? authorSnapshot = null)
    {
        var src = FindProfile(config, profileId);
        if (src is null) return (config, null);
        var now = UtcNowIsoFactory();
        var newPhases = new List<RaidProfilePhase>(src.Phases.Count);
        foreach (var ph in src.Phases)
        {
            var newTimelines = ph.Timelines.Select(tl => tl with { Id = NewIdFactory("tl") }).ToList();
            newPhases.Add(ph with { Id = NewIdFactory("phase"), Timelines = newTimelines });
        }
        var author = authorSnapshot is { } a ? NormalizeAuthor(a) : src.AuthorSnapshot;
        var cloned = src with
        {
            Id = NewIdFactory("boss"),
            ProfileName = $"{src.ProfileName} Copy",
            Source = "local",
            RemoteId = null,
            CreatedAt = now,
            UpdatedAt = now,
            Phases = newPhases,
            AuthorSnapshot = author,
        };
        return (UpsertProfile(config, cloned, activate: false), cloned);
    }

    public static RaidProfileSummary SummarizeProfile(RaidProfile p) => new(
        Id: p.Id,
        ProfileName: p.ProfileName,
        Description: p.Description,
        BossTotalHp: p.BossTotalHp,
        EnrageTimeS: p.EnrageTimeS,
        SimpleMode: p.SimpleMode,
        PhaseCount: p.Phases.Count,
        TimelineCount: p.Phases.Sum(ph => ph.Timelines.Count),
        Source: p.Source,
        RemoteId: p.RemoteId,
        UpdatedAt: p.UpdatedAt);

    public static string ExportProfileJson(RaidProfile profile)
    {
        var payload = new { schema_version = SchemaVersion, profile };
        return JsonSerializer.Serialize(payload, new JsonSerializerOptions
        {
            WriteIndented = true,
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
            PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        });
    }

    // ── Internal plumbing ────────────────────────────────────────────────

    private static readonly JsonElement EmptyObject = JsonDocument.Parse("{}").RootElement.Clone();

    private static string NonEmpty(string s, string fallback) =>
        string.IsNullOrEmpty(s) ? fallback : s;

    private static JsonElement WrapAuthor(RaidAuthor a)
    {
        var json = JsonSerializer.Serialize(new
        {
            player_uid = a.PlayerUid,
            player_name = a.PlayerName,
            profession_id = a.ProfessionId,
            profession_name = a.ProfessionName,
        });
        return JsonDocument.Parse(json).RootElement.Clone();
    }
}

internal static class JsonElementExtensions
{
    public static JsonElement GetOrUndefined(this JsonElement el, string key)
    {
        if (el.ValueKind == JsonValueKind.Object && el.TryGetProperty(key, out var v))
            return v;
        return default;
    }
}

public sealed record RaidTimelineCondition(string Type, string Comparator, double Value);

public sealed record RaidTimelineEntry(
    string Id,
    double TimeSeconds,
    string Label,
    string AlertType,
    double RepeatIntervalS,
    double PreWarnS,
    double DurationS,
    RaidTimelineCondition? Condition);

public sealed record RaidPhaseTrigger(string Type, double Value);

public sealed record RaidProfilePhase(
    string Id,
    string Name,
    RaidPhaseTrigger Trigger,
    IReadOnlyList<RaidTimelineEntry> Timelines);

public sealed record RaidAuthor(
    string PlayerUid,
    string PlayerName,
    int ProfessionId,
    string ProfessionName);

public sealed record RaidProfile(
    string Id,
    int SchemaVersion,
    string ProfileName,
    string Description,
    long BossTotalHp,
    int EnrageTimeS,
    bool SimpleMode,
    string TargetNamePattern,
    IReadOnlyList<RaidProfilePhase> Phases,
    string Source,
    string? RemoteId,
    string CreatedAt,
    string UpdatedAt,
    RaidAuthor AuthorSnapshot);

public sealed record RaidProfileSummary(
    string Id,
    string ProfileName,
    string Description,
    long BossTotalHp,
    int EnrageTimeS,
    bool SimpleMode,
    int PhaseCount,
    int TimelineCount,
    string Source,
    string? RemoteId,
    string UpdatedAt);

public sealed record RaidRemoteQuery(string Q, int Page, int PageSize);

public sealed record RaidRemoteSearch(
    RaidRemoteQuery Query,
    IReadOnlyList<JsonElement> Results,
    string Error,
    string FetchedAt);

public sealed record RaidConfig(
    bool Enabled,
    string ActiveProfileId,
    string ServerUrl,
    IReadOnlyList<RaidProfile> Profiles,
    RaidRemoteSearch LastRemoteSearch);
