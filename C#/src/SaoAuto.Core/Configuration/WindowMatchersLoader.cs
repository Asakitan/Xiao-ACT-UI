using System.Text.Json.Nodes;
using SaoAuto.Core.Vision;

namespace SaoAuto.Core.Configuration;

/// <summary>
/// S100 — Settings-driven override for the
/// <see cref="WindowLocator"/> match lists.
///
/// Python's <c>config.GAME_WINDOW_KEYWORDS</c> /
/// <c>GAME_PROCESS_NAMES</c> are module constants — a renamed game
/// executable requires a code edit. The C# port adds a thin settings
/// hook so an operator can override either list via
/// <c>game_window.keywords</c> / <c>game_window.process_names</c>
/// without rebuilding. Falls through to
/// <see cref="GameWindowConfig.DefaultTitleKeywords"/> /
/// <see cref="GameWindowConfig.DefaultProcessNames"/> when missing,
/// empty, or non-array.
///
/// String values are trimmed; empty/whitespace-only entries are
/// dropped (matches the spirit of Python's <c>list(filter(None, …))</c>
/// patterns elsewhere). Process names are not lowercased here —
/// <see cref="WindowLocator"/> normalises on its own.
/// </summary>
public static class WindowMatchersLoader
{
    public static IReadOnlyList<string> LoadKeywords(SettingsManager settings)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        var raw = settings.Get<JsonObject?>("game_window")?["keywords"] as JsonArray;
        var parsed = ParseStringArray(raw);
        return parsed.Count == 0 ? GameWindowConfig.DefaultTitleKeywords : parsed;
    }

    public static IReadOnlyList<string> LoadProcessNames(SettingsManager settings)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        var raw = settings.Get<JsonObject?>("game_window")?["process_names"] as JsonArray;
        var parsed = ParseStringArray(raw);
        return parsed.Count == 0 ? GameWindowConfig.DefaultProcessNames : parsed;
    }

    public static IReadOnlyList<string> ParseStringArray(JsonArray? raw)
    {
        if (raw is null || raw.Count == 0) return Array.Empty<string>();
        var result = new List<string>(raw.Count);
        foreach (var node in raw)
        {
            if (node is null) continue;
            string? value;
            try { value = node.GetValue<string>(); }
            catch { continue; }
            if (string.IsNullOrWhiteSpace(value)) continue;
            result.Add(value.Trim());
        }
        return result;
    }
}
