using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.Core.Updater;

namespace SaoAuto.UpdateHost;

/// <summary>
/// File-backed manifest store. The on-disk shape is a JSON object keyed by
/// "<channel>|<target>", values are <see cref="UpdateManifest"/> JSON.
/// All mutations save synchronously under the lock; reads are lock-free
/// snapshots so the HTTP path stays responsive.
/// </summary>
public sealed class ManifestStore
{
    private readonly object _gate = new();
    private readonly string _path;
    private Dictionary<string, UpdateManifest> _byKey = new(StringComparer.Ordinal);

    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        Converters = { new System.Text.Json.Serialization.JsonStringEnumConverter() },
    };

    public ManifestStore(string? path = null)
    {
        _path = path ?? Environment.GetEnvironmentVariable("SAO_MANIFEST_PATH")
            ?? System.IO.Path.Combine(AppContext.BaseDirectory, "manifests.json");
        Load();
    }

    public string Path => _path;

    public UpdateManifest? Latest(string channel, string target)
    {
        var key = Key(channel, target);
        Dictionary<string, UpdateManifest> snapshot;
        lock (_gate) snapshot = _byKey;
        return snapshot.TryGetValue(key, out var m) ? m : null;
    }

    public void Publish(UpdateManifest manifest)
    {
        var key = Key(manifest.Channel, manifest.Target);
        lock (_gate)
        {
            var next = new Dictionary<string, UpdateManifest>(_byKey, StringComparer.Ordinal)
            {
                [key] = manifest,
            };
            _byKey = next;
            Save();
        }
    }

    public IReadOnlyCollection<UpdateManifest> Summary()
    {
        Dictionary<string, UpdateManifest> snapshot;
        lock (_gate) snapshot = _byKey;
        return snapshot.Values.ToArray();
    }

    private static string Key(string channel, string target) => $"{channel}|{target}";

    private void Load()
    {
        try
        {
            if (!File.Exists(_path)) return;
            var text = File.ReadAllText(_path);
            if (string.IsNullOrWhiteSpace(text)) return;
            var root = JsonNode.Parse(text)?.AsObject();
            if (root is null) return;
            var dict = new Dictionary<string, UpdateManifest>(StringComparer.Ordinal);
            foreach (var kv in root)
            {
                var m = JsonSerializer.Deserialize<UpdateManifest>(kv.Value!.ToJsonString(), JsonOpts);
                if (m is not null) dict[kv.Key] = m;
            }
            _byKey = dict;
        }
        catch { /* corrupt file = empty store; first publish overwrites */ }
    }

    private void Save()
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(_path) ?? ".");
        var root = new JsonObject();
        foreach (var kv in _byKey)
        {
            root[kv.Key] = JsonNode.Parse(JsonSerializer.Serialize(kv.Value, JsonOpts));
        }
        var tmp = _path + ".tmp";
        File.WriteAllText(tmp, root.ToJsonString(JsonOpts));
        if (File.Exists(_path)) File.Replace(tmp, _path, null, ignoreMetadataErrors: true);
        else File.Move(tmp, _path);
    }
}
