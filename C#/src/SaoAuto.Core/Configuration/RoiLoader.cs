using System.Text.Json.Nodes;
using SaoAuto.Core.Vision;

namespace SaoAuto.Core.Configuration;

/// <summary>
/// S95 — Read named ROI rectangles from settings, falling back to the
/// canonical defaults baked into Python <c>config.DEFAULT_ROI</c>
/// (config.py 1191–1201). Mirrors <c>SettingsManager.get_roi(name)</c>
/// (config.py 1463–1469): user override wins when the
/// <c>settings["roi"][name]</c> sub-object is present and truthy
/// (non-empty), otherwise the in-process default applies.
///
/// "Truthy" follows Python: an empty <c>{}</c> override falls through
/// to the default. Mirrors <c>if custom:</c>.
/// </summary>
public static class RoiLoader
{
    /// <summary>
    /// Default percentage ROIs from Python <c>config.DEFAULT_ROI</c>.
    /// Skill bar deliberately omitted — Python sources it from
    /// <c>get_skill_bar_roi()</c> (a derived layout helper), which the
    /// C# port handles via <see cref="SkillSlotRoiTable"/>.
    /// </summary>
    public static readonly IReadOnlyDictionary<string, Roi> Defaults =
        new Dictionary<string, Roi>(StringComparer.Ordinal)
        {
            ["identity"] = new Roi(0.010, 0.910, 0.200, 0.060),
            ["level"] = new Roi(0.010, 0.925, 0.100, 0.040),
            ["name"] = new Roi(0.085, 0.930, 0.120, 0.030),
            ["hp_bar"] = new Roi(0.330, 0.932, 0.340, 0.036),
            ["hp_text"] = new Roi(0.380, 0.940, 0.240, 0.028),
            ["stamina_bar"] = new Roi(0.330, 0.957, 0.340, 0.036),
            ["stamina_text"] = new Roi(0.530, 0.968, 0.130, 0.018),
            ["player_id"] = new Roi(0.230, 0.968, 0.100, 0.020),
        };

    public static Roi Load(SettingsManager settings, string name)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        if (string.IsNullOrEmpty(name)) throw new ArgumentException("name required", nameof(name));

        var roiTable = settings.Get<JsonObject?>(SettingsKeys.Roi);
        if (roiTable is not null
            && roiTable.TryGetPropertyValue(name, out var node)
            && TryParseRoi(node, out var custom))
        {
            return custom;
        }
        return Defaults.TryGetValue(name, out var fallback) ? fallback : default;
    }

    public static bool TryParseRoi(JsonNode? node, out Roi roi)
    {
        roi = default;
        if (node is not JsonObject obj || obj.Count == 0) return false;
        if (!TryReadDouble(obj, "x", out var x)) return false;
        if (!TryReadDouble(obj, "y", out var y)) return false;
        if (!TryReadDouble(obj, "w", out var w)) return false;
        if (!TryReadDouble(obj, "h", out var h)) return false;
        roi = new Roi(x, y, w, h);
        return true;
    }

    private static bool TryReadDouble(JsonObject obj, string key, out double value)
    {
        value = 0;
        if (!obj.TryGetPropertyValue(key, out var node) || node is not JsonValue v) return false;
        if (v.TryGetValue<double>(out value)) return true;
        if (v.TryGetValue<int>(out var i)) { value = i; return true; }
        if (v.TryGetValue<long>(out var l)) { value = l; return true; }
        return false;
    }
}
