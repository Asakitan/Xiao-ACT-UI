using System.Text.Json;
using System.Text.Json.Nodes;

namespace SaoAuto.ParityTests;

/// <summary>
/// Helper to load `tools/parity-fixtures/*.json` (Python-generated expected
/// snapshots) for cross-runtime comparison. Each fixture is a JSON object
/// with at least a `kind` discriminator and a `data` payload.
/// </summary>
public static class ParityFixture
{
    public static string FixturesRoot => Path.Combine(
        AppContext.BaseDirectory, "Fixtures");

    public static JsonObject Load(string relativePath)
    {
        var fullPath = Path.Combine(FixturesRoot, relativePath);
        if (!File.Exists(fullPath))
        {
            throw new FileNotFoundException(
                $"Parity fixture not found at {fullPath}. Generate it via tools/parity-fixtures/*.py.");
        }
        var text = File.ReadAllText(fullPath);
        var node = JsonNode.Parse(text);
        return node?.AsObject() ?? throw new InvalidDataException($"Fixture is not a JSON object: {fullPath}");
    }

    public static IEnumerable<string> ListAvailable()
    {
        if (!Directory.Exists(FixturesRoot)) yield break;
        foreach (var f in Directory.EnumerateFiles(FixturesRoot, "*.json", SearchOption.AllDirectories))
        {
            yield return Path.GetRelativePath(FixturesRoot, f);
        }
    }
}

/// <summary>
/// Compare two `JsonNode`s tolerantly: numbers compared with epsilon,
/// objects field-by-field, arrays element-wise. Returns the first
/// divergence path or null when equal.
/// </summary>
public static class JsonParity
{
    public static string? FirstDifference(JsonNode? expected, JsonNode? actual, double numericEpsilon = 0)
    {
        return FirstDifference(expected, actual, "", numericEpsilon);
    }

    private static string? FirstDifference(JsonNode? expected, JsonNode? actual, string path, double epsilon)
    {
        if (expected is null && actual is null) return null;
        if (expected is null || actual is null) return $"{path}: null mismatch (expected={expected}, actual={actual})";

        if (expected is JsonObject eo && actual is JsonObject ao)
        {
            foreach (var (key, value) in eo)
            {
                if (!ao.TryGetPropertyValue(key, out var aVal))
                {
                    return $"{path}.{key}: missing in actual";
                }
                var sub = FirstDifference(value, aVal, $"{path}.{key}", epsilon);
                if (sub is not null) return sub;
            }
            foreach (var (key, _) in ao)
            {
                if (!eo.ContainsKey(key))
                {
                    return $"{path}.{key}: extra in actual";
                }
            }
            return null;
        }

        if (expected is JsonArray ea && actual is JsonArray aa)
        {
            if (ea.Count != aa.Count) return $"{path}: length mismatch ({ea.Count} vs {aa.Count})";
            for (var i = 0; i < ea.Count; i++)
            {
                var sub = FirstDifference(ea[i], aa[i], $"{path}[{i}]", epsilon);
                if (sub is not null) return sub;
            }
            return null;
        }

        if (expected is JsonValue ev && actual is JsonValue av)
        {
            if (epsilon > 0 && ev.TryGetValue<double>(out var e) && av.TryGetValue<double>(out var a))
            {
                return Math.Abs(e - a) <= epsilon ? null : $"{path}: numeric drift |{e}-{a}|>{epsilon}";
            }
            var es = ev.ToJsonString();
            var aas = av.ToJsonString();
            return es == aas ? null : $"{path}: {es} vs {aas}";
        }

        return $"{path}: type mismatch";
    }
}
