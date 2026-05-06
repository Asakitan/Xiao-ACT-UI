using System.Text.Json;
using System.Text.Json.Nodes;

namespace SaoAuto.MemProbe;

/// <summary>
/// Loads / persists <c>mem_probe/anchors.json</c>. The file shape mixes
/// flat schema-v1 keys (<c>last_self_obj</c>, <c>last_uid_off</c>, …)
/// with a nested schema-v2 (<c>anchors.&lt;name&gt;</c>) — both are
/// preserved verbatim so unknown keys round-trip. Used by the
/// <see cref="UnifiedMemorySource"/> to seed each watcher's read recipe
/// from a previous successful locate (warm-start path).
/// </summary>
public sealed class MemoryAnchorStore
{
    public JsonObject Root { get; }
    public string? Path { get; }

    private static readonly JsonSerializerOptions WriteOpts = new() { WriteIndented = true };

    public MemoryAnchorStore(JsonObject root, string? path = null)
    {
        Root = root;
        Path = path;
    }

    public static MemoryAnchorStore Empty() => new(new JsonObject());

    public static MemoryAnchorStore Load(string path)
    {
        if (!File.Exists(path)) return new MemoryAnchorStore(new JsonObject(), path);
        try
        {
            using var fs = File.OpenRead(path);
            var node = JsonNode.Parse(fs);
            return new MemoryAnchorStore(node?.AsObject() ?? new JsonObject(), path);
        }
        catch { return new MemoryAnchorStore(new JsonObject(), path); }
    }

    public void Save(string? path = null)
    {
        var target = path ?? Path ?? throw new InvalidOperationException("No path bound");
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(target))!);
        File.WriteAllText(target, Root.ToJsonString(WriteOpts));
    }

    public JsonObject SmartLocator => GetOrCreate(Root, "smart_locator");

    /// <summary>v2 anchor block (e.g. "self", "scene_manager", "entity_collection").</summary>
    public JsonObject? GetV2Anchor(string name)
    {
        var sl = Root["smart_locator"]?.AsObject();
        var nested = sl?["anchors"]?.AsObject();
        return nested?[name]?.AsObject();
    }

    private static JsonObject GetOrCreate(JsonObject parent, string key)
    {
        if (parent[key] is JsonObject existing) return existing;
        var fresh = new JsonObject();
        parent[key] = fresh;
        return fresh;
    }

    public static ulong ParseHex(JsonNode? node)
    {
        if (node is null) return 0;
        if (node is JsonValue jv)
        {
            if (jv.TryGetValue<long>(out var l)) return (ulong)l;
            if (jv.TryGetValue<ulong>(out var u)) return u;
            if (jv.TryGetValue<string>(out var s) && !string.IsNullOrEmpty(s))
            {
                var trimmed = s.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? s[2..] : s;
                if (ulong.TryParse(trimmed, System.Globalization.NumberStyles.HexNumber,
                    System.Globalization.CultureInfo.InvariantCulture, out var hex)) return hex;
            }
        }
        return 0;
    }

    public static int GetInt(JsonNode? node, int fallback = -1)
    {
        if (node is JsonValue jv)
        {
            if (jv.TryGetValue<int>(out var i)) return i;
            if (jv.TryGetValue<long>(out var l)) return (int)l;
            if (jv.TryGetValue<string>(out var s) && int.TryParse(s, out var p)) return p;
        }
        return fallback;
    }

    public static string? GetStr(JsonNode? node) =>
        node is JsonValue jv && jv.TryGetValue<string>(out var s) ? s : null;
}
