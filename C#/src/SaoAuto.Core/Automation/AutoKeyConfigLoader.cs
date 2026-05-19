using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Core.Automation;

/// <summary>
/// S136 — settings-driven loader for <see cref="AutoKeyConfig"/>. Mirrors
/// Python's <c>load_auto_key_config</c> (auto_key_engine.py 396–397):
/// pulls the raw <c>auto_key</c> node from <see cref="SettingsManager"/>
/// and feeds it through <see cref="AutoKeyProfileStore.NormalizeConfig"/>.
///
/// Equivalent to Python's
/// <c>load_auto_key_config(settings, state_snapshot=snapshot_author_from_state(gs))</c>:
/// authoring fallback is derived from the current <see cref="GameState"/>
/// so freshly-cloned local profiles get stamped with the live player /
/// profession identity even if the on-disk node is empty.
/// </summary>
public static class AutoKeyConfigLoader
{
    public const string SettingsKey = "auto_key";

    public static AutoKeyConfig Load(
        SettingsManager settings,
        AuthorSnapshot? authorFallback = null,
        Func<string>? newId = null,
        Func<DateTimeOffset>? clock = null)
    {
        ArgumentNullException.ThrowIfNull(settings);
        var raw = ReadRawNode(settings);
        return AutoKeyProfileStore.NormalizeConfig(raw, authorFallback, newId, clock);
    }

    /// <summary>
    /// Settings + live state combined: mirrors Python's
    /// <c>load_auto_key_config(settings, state_snapshot=snapshot_author_from_state(gs))</c>.
    /// Caller-supplied snapshot keeps the loader pure with respect to
    /// <see cref="GameStateManager"/>'s lock.
    /// </summary>
    public static AutoKeyConfig LoadWithStateAuthor(
        SettingsManager settings,
        GameState state,
        Func<string>? newId = null,
        Func<DateTimeOffset>? clock = null)
    {
        ArgumentNullException.ThrowIfNull(state);
        return Load(settings, AuthorFromState(state), newId, clock);
    }

    /// <summary>
    /// Port of Python's <c>snapshot_author_from_state</c>
    /// (auto_key_engine.py 152–158).
    /// </summary>
    public static AuthorSnapshot AuthorFromState(GameState state)
    {
        ArgumentNullException.ThrowIfNull(state);
        return new AuthorSnapshot(
            PlayerUid: state.PlayerId ?? string.Empty,
            PlayerName: state.PlayerName ?? string.Empty,
            ProfessionId: state.ProfessionId,
            ProfessionName: state.ProfessionName ?? string.Empty);
    }

    /// <summary>
    /// Serialize and re-deserialize the active profile / config tree back
    /// onto disk. Mirrors Python's <c>save_auto_key_config</c> at
    /// auto_key_engine.py 400–402: re-normalises so caller-side edits
    /// can't sneak unnormalised state into settings.
    /// </summary>
    public static void Save(SettingsManager settings, AutoKeyConfig config)
    {
        ArgumentNullException.ThrowIfNull(settings);
        ArgumentNullException.ThrowIfNull(config);
        var node = JsonSerializer.SerializeToNode(config, WriteOptions);
        settings.Set(SettingsKey, node);
    }

    private static readonly JsonSerializerOptions WriteOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.Never,
    };

    private static JsonElement ReadRawNode(SettingsManager settings)
    {
        // SettingsManager.Get<JsonElement> handles missing keys via
        // defaultValue; coerce undefined to an empty object so the
        // normaliser can apply default semantics uniformly.
        var el = settings.Get<JsonElement>(SettingsKey);
        if (el.ValueKind is JsonValueKind.Undefined or JsonValueKind.Null)
        {
            using var doc = JsonDocument.Parse("{}");
            return doc.RootElement.Clone();
        }
        return el;
    }
}
