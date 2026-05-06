using System.Collections.Immutable;
using System.Text.Json;

namespace SaoAuto.Core.Automation;

/// <summary>
/// S73 — auto_key profile spec / normalization layer. C# port of
/// <c>auto_key_engine.py</c> lines 208–336 (defaults +
/// <c>normalize_condition</c> / <c>normalize_action</c> /
/// <c>normalize_profile</c>).
///
/// "Spec" types describe the on-disk JSON shape that profile editors
/// produce and the runtime later compiles into the existing
/// <see cref="AutoKeyAction"/> / <see cref="AutoKeyTrigger"/> ADT.
/// Kept separate from the runtime types so this normalization layer
/// can land without touching the executor.
///
/// Coercion semantics mirror Python verbatim: out-of-range numerics
/// clamp, unknown enums drop to defaults, unknown condition types
/// drop the condition, missing actions get a single default action.
/// </summary>
public static class AutoKeyProfileSpec
{
    public const int SchemaVersion = 1;
    public const string DefaultServerUrl = "http://47.82.157.220:9320";

    public static string DefaultKeyForSlot(int slotIndex) =>
        slotIndex.ToString(System.Globalization.CultureInfo.InvariantCulture);

    public static string Slugify(string? text)
    {
        var value = (text ?? string.Empty).Trim().Replace(' ', '_');
        if (value.Length == 0) value = "auto_key_profile";
        var sb = new System.Text.StringBuilder(value.Length);
        foreach (var ch in value)
        {
            if (char.IsLetterOrDigit(ch) || ch is '-' or '_') sb.Append(ch);
            else if (ch >= '\u4e00' && ch <= '\u9fff') sb.Append(ch);
            else sb.Append('_');
        }
        var trimmed = sb.ToString().Trim('_');
        return trimmed.Length == 0 ? "auto_key_profile" : trimmed;
    }

    public static string NewId(string prefix, Func<string>? randomHex = null)
    {
        randomHex ??= () => Guid.NewGuid().ToString("N").Substring(0, 12);
        return $"{prefix}_{randomHex()}";
    }

    public static string UtcNowIso(Func<DateTimeOffset>? clock = null)
    {
        var now = (clock ?? (() => DateTimeOffset.UtcNow))();
        return now.UtcDateTime.ToString("yyyy-MM-ddTHH:mm:ssZ", System.Globalization.CultureInfo.InvariantCulture);
    }

    public static AuthorSnapshot NormalizeAuthor(JsonElement raw)
    {
        return new AuthorSnapshot(
            PlayerUid: ReadString(raw, "player_uid", "uid"),
            PlayerName: ReadString(raw, "player_name", "name"),
            ProfessionId: Math.Max(0, ReadInt(raw, "profession_id", 0)),
            ProfessionName: ReadString(raw, "profession_name", "profession"));
    }

    public static AutoKeyActionSpec MakeDefaultAction(int slotIndex = 1, Func<string>? newId = null)
    {
        slotIndex = Clamp(slotIndex, 1, 9);
        return new AutoKeyActionSpec(
            Id: NewId("action", newId),
            Label: $"Action {slotIndex}",
            Enabled: true,
            SlotIndex: slotIndex,
            Key: DefaultKeyForSlot(slotIndex),
            PressMode: "tap",
            PressCount: 1,
            PressIntervalMs: 40,
            HoldMs: 80,
            ReadyDelayMs: 0,
            MinRearmMs: 800,
            PostDelayMs: 120,
            Conditions: ImmutableArray<AutoKeyCondition>.Empty);
    }

    public static AutoKeyProfileSpecRecord MakeDefaultProfile(
        AuthorSnapshot? author = null,
        Func<string>? newId = null,
        Func<DateTimeOffset>? clock = null)
    {
        author ??= AuthorSnapshot.Empty;
        var nowIso = UtcNowIso(clock);
        return new AutoKeyProfileSpecRecord(
            Id: NewId("profile", newId),
            SchemaVersion: SchemaVersion,
            ProfileName: "New Auto Key Profile",
            Description: "",
            ProfessionId: Math.Max(0, author.ProfessionId),
            ProfessionName: author.ProfessionName,
            Source: "local",
            RemoteId: null,
            CreatedAt: nowIso,
            UpdatedAt: nowIso,
            AuthorSnapshot: author,
            Engine: new AutoKeyEngineConfig(TickMs: 50, RequireForeground: true, PauseOnDeath: true),
            Actions: ImmutableArray.Create(MakeDefaultAction(1, newId)));
    }

    public static AutoKeyCondition? NormalizeCondition(JsonElement raw)
    {
        if (raw.ValueKind != JsonValueKind.Object) return null;
        var type = ReadString(raw, "type").ToLowerInvariant();
        return type switch
        {
            "hp_pct_gte" => new HpPctGteCondition(ClampF(ReadDouble(raw, "value", 0.0), 0.0, 1.0)),
            "hp_pct_lte" => new HpPctLteCondition(ClampF(ReadDouble(raw, "value", 0.0), 0.0, 1.0)),
            "sta_pct_gte" => new StaPctGteCondition(ClampF(ReadDouble(raw, "value", 0.0), 0.0, 1.0)),
            "burst_ready_is" => new BurstReadyIsCondition(ReadBool(raw, "value", false)),
            "slot_state_is" => new SlotStateIsCondition(
                Clamp(ReadInt(raw, "slot_index", 0), 0, 9),
                NonEmptyOr(ReadString(raw, "state").ToLowerInvariant(), "ready")),
            "profession_is" => new ProfessionIsCondition(ReadString(raw, "value")),
            "player_name_is" => new PlayerNameIsCondition(ReadString(raw, "value")),
            "in_combat_is" => new InCombatIsCondition(ReadBool(raw, "value", true)),
            _ => null,
        };
    }

    public static AutoKeyActionSpec NormalizeAction(JsonElement raw, int fallbackSlot = 1, Func<string>? newId = null)
    {
        var slot = Clamp(ReadInt(raw, "slot_index", fallbackSlot), 1, 9);
        var pressMode = ReadString(raw, "press_mode").ToLowerInvariant();
        if (pressMode is not ("tap" or "hold")) pressMode = "tap";
        var conds = ImmutableArray.CreateBuilder<AutoKeyCondition>();
        if (raw.ValueKind == JsonValueKind.Object && raw.TryGetProperty("conditions", out var cArr) && cArr.ValueKind == JsonValueKind.Array)
        {
            foreach (var c in cArr.EnumerateArray())
            {
                var n = NormalizeCondition(c);
                if (n is not null) conds.Add(n);
            }
        }
        var keyRaw = ReadString(raw, "key");
        var key = (keyRaw.Length > 0 ? keyRaw : DefaultKeyForSlot(slot)).ToUpperInvariant();
        return new AutoKeyActionSpec(
            Id: NonEmptyOr(ReadString(raw, "id"), NewId("action", newId)),
            Label: NonEmptyOr(ReadString(raw, "label"), $"Action {slot}"),
            Enabled: ReadBool(raw, "enabled", true),
            SlotIndex: slot,
            Key: key,
            PressMode: pressMode,
            PressCount: Clamp(ReadInt(raw, "press_count", 1), 1, 20),
            PressIntervalMs: Clamp(ReadInt(raw, "press_interval_ms", 40), 0, 10_000),
            HoldMs: Clamp(ReadInt(raw, "hold_ms", 80), 0, 10_000),
            ReadyDelayMs: Clamp(ReadInt(raw, "ready_delay_ms", 0), 0, 60_000),
            MinRearmMs: Clamp(ReadInt(raw, "min_rearm_ms", 800), 0, 120_000),
            PostDelayMs: Clamp(ReadInt(raw, "post_delay_ms", 120), 0, 120_000),
            Conditions: conds.ToImmutable());
    }

    public static AutoKeyProfileSpecRecord NormalizeProfile(
        JsonElement raw,
        AuthorSnapshot? authorFallback = null,
        string? sourceOverride = null,
        Func<string>? newId = null,
        Func<DateTimeOffset>? clock = null)
    {
        var defaults = MakeDefaultProfile(authorFallback, newId, clock);
        var src = NonEmptyOr((sourceOverride ?? ReadString(raw, "source")).ToLowerInvariant(), defaults.Source);
        if (src is not ("local" or "downloaded" or "uploaded")) src = "local";

        AuthorSnapshot author;
        if (raw.ValueKind == JsonValueKind.Object && raw.TryGetProperty("author_snapshot", out var aSnap) && aSnap.ValueKind == JsonValueKind.Object)
            author = NormalizeAuthor(aSnap);
        else
            author = authorFallback ?? AuthorSnapshot.Empty;

        var professionName = NonEmptyOr(ReadString(raw, "profession_name"), author.ProfessionName);
        var professionId = Math.Max(0,
            ReadInt(raw, "profession_id", 0) > 0 ? ReadInt(raw, "profession_id", 0) : author.ProfessionId);

        var actions = ImmutableArray.CreateBuilder<AutoKeyActionSpec>();
        if (raw.ValueKind == JsonValueKind.Object && raw.TryGetProperty("actions", out var aArr) && aArr.ValueKind == JsonValueKind.Array)
        {
            int idx = 1;
            foreach (var item in aArr.EnumerateArray())
            {
                actions.Add(NormalizeAction(item, idx, newId));
                idx++;
            }
        }
        if (actions.Count == 0) actions.Add(MakeDefaultAction(1, newId));

        AutoKeyEngineConfig engine;
        if (raw.ValueKind == JsonValueKind.Object && raw.TryGetProperty("engine", out var eObj) && eObj.ValueKind == JsonValueKind.Object)
        {
            engine = new AutoKeyEngineConfig(
                TickMs: Clamp(ReadInt(eObj, "tick_ms", 50), 10, 1000),
                RequireForeground: ReadBool(eObj, "require_foreground", true),
                PauseOnDeath: ReadBool(eObj, "pause_on_death", true));
        }
        else
        {
            engine = defaults.Engine;
        }

        return new AutoKeyProfileSpecRecord(
            Id: NonEmptyOr(ReadString(raw, "id"), defaults.Id),
            SchemaVersion: SchemaVersion,
            ProfileName: NonEmptyOr(ReadString(raw, "profile_name"), defaults.ProfileName),
            Description: ReadString(raw, "description"),
            ProfessionId: professionId,
            ProfessionName: professionName,
            Source: src,
            RemoteId: NullIfEmpty(ReadString(raw, "remote_id")),
            CreatedAt: NonEmptyOr(ReadString(raw, "created_at"), defaults.CreatedAt),
            UpdatedAt: UtcNowIso(clock),
            AuthorSnapshot: author,
            Engine: engine,
            Actions: actions.ToImmutable());
    }

    // ── coercion helpers ───────────────────────────────────────────

    private static int Clamp(int v, int lo, int hi) => v < lo ? lo : (v > hi ? hi : v);
    private static double ClampF(double v, double lo, double hi) => v < lo ? lo : (v > hi ? hi : v);
    private static string NonEmptyOr(string s, string fallback) => string.IsNullOrEmpty(s) ? fallback : s;
    private static string? NullIfEmpty(string s) => string.IsNullOrEmpty(s) ? null : s;

    private static string ReadString(JsonElement obj, string key, string? altKey = null)
    {
        if (obj.ValueKind != JsonValueKind.Object) return string.Empty;
        if (obj.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.String) return (v.GetString() ?? string.Empty).Trim();
        if (altKey != null && obj.TryGetProperty(altKey, out var v2) && v2.ValueKind == JsonValueKind.String) return (v2.GetString() ?? string.Empty).Trim();
        return string.Empty;
    }

    private static int ReadInt(JsonElement obj, string key, int @default)
    {
        if (obj.ValueKind != JsonValueKind.Object || !obj.TryGetProperty(key, out var v)) return @default;
        return v.ValueKind switch
        {
            JsonValueKind.Number => v.TryGetInt32(out var i) ? i : (v.TryGetInt64(out var l) ? (int)l : (v.TryGetDouble(out var d) ? (int)d : @default)),
            JsonValueKind.String => int.TryParse(v.GetString(), System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out var s) ? s : @default,
            JsonValueKind.True => 1,
            JsonValueKind.False => 0,
            _ => @default,
        };
    }

    private static double ReadDouble(JsonElement obj, string key, double @default)
    {
        if (obj.ValueKind != JsonValueKind.Object || !obj.TryGetProperty(key, out var v)) return @default;
        return v.ValueKind switch
        {
            JsonValueKind.Number => v.TryGetDouble(out var d) ? d : @default,
            JsonValueKind.String => double.TryParse(v.GetString(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var s) ? s : @default,
            JsonValueKind.True => 1.0,
            JsonValueKind.False => 0.0,
            _ => @default,
        };
    }

    private static bool ReadBool(JsonElement obj, string key, bool @default)
    {
        if (obj.ValueKind != JsonValueKind.Object || !obj.TryGetProperty(key, out var v)) return @default;
        return v.ValueKind switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            JsonValueKind.Number => v.TryGetDouble(out var d) && d != 0.0,
            JsonValueKind.String => (v.GetString() ?? string.Empty).Trim().ToLowerInvariant() switch
            {
                "1" or "true" or "yes" or "on" => true,
                "0" or "false" or "no" or "off" => false,
                _ => @default,
            },
            _ => @default,
        };
    }
}

public sealed record AuthorSnapshot(
    string PlayerUid,
    string PlayerName,
    int ProfessionId,
    string ProfessionName)
{
    public static readonly AuthorSnapshot Empty = new("", "", 0, "");
}

public sealed record AutoKeyEngineConfig(
    int TickMs,
    bool RequireForeground,
    bool PauseOnDeath);

public sealed record AutoKeyActionSpec(
    string Id,
    string Label,
    bool Enabled,
    int SlotIndex,
    string Key,
    string PressMode,
    int PressCount,
    int PressIntervalMs,
    int HoldMs,
    int ReadyDelayMs,
    int MinRearmMs,
    int PostDelayMs,
    ImmutableArray<AutoKeyCondition> Conditions);

public sealed record AutoKeyProfileSpecRecord(
    string Id,
    int SchemaVersion,
    string ProfileName,
    string Description,
    int ProfessionId,
    string ProfessionName,
    string Source,
    string? RemoteId,
    string CreatedAt,
    string UpdatedAt,
    AuthorSnapshot AuthorSnapshot,
    AutoKeyEngineConfig Engine,
    ImmutableArray<AutoKeyActionSpec> Actions);

public abstract record AutoKeyCondition;
public sealed record HpPctGteCondition(double Value) : AutoKeyCondition;
public sealed record HpPctLteCondition(double Value) : AutoKeyCondition;
public sealed record StaPctGteCondition(double Value) : AutoKeyCondition;
public sealed record BurstReadyIsCondition(bool Value) : AutoKeyCondition;
public sealed record SlotStateIsCondition(int SlotIndex, string State) : AutoKeyCondition;
public sealed record ProfessionIsCondition(string Value) : AutoKeyCondition;
public sealed record PlayerNameIsCondition(string Value) : AutoKeyCondition;
public sealed record InCombatIsCondition(bool Value) : AutoKeyCondition;
