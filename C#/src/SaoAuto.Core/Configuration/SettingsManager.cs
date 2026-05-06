using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.Configuration;

/// <summary>
/// Loads and saves <c>settings.json</c> while preserving keys the C# port does not yet model.
/// Mirrors the Python <c>config.SettingsManager</c> contract: legacy keys pruned only on save,
/// atomic write via temp file + replace, unknown keys round-trip untouched.
/// </summary>
public sealed class SettingsManager
{
    private static readonly JsonSerializerOptions ReadOptions = new()
    {
        AllowTrailingCommas = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
    };

    private static readonly JsonSerializerOptions WriteOptions = new()
    {
        WriteIndented = true,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    private readonly object _gate = new();
    private readonly string _path;
    private readonly ILogger _log;
    private JsonObject _data = new();

    public SettingsManager(string path, ILogger? logger = null)
    {
        _path = path ?? throw new ArgumentNullException(nameof(path));
        _log = logger ?? NullLogger.Instance;
        Load();
    }

    public string Path => _path;

    public void Load()
    {
        lock (_gate)
        {
            CleanupStaleTempFiles();
            try
            {
                if (File.Exists(_path))
                {
                    var text = File.ReadAllText(_path);
                    var node = JsonNode.Parse(text, documentOptions: new JsonDocumentOptions
                    {
                        AllowTrailingCommas = true,
                        CommentHandling = JsonCommentHandling.Skip,
                    });
                    _data = node as JsonObject ?? new JsonObject();
                }
                else
                {
                    _data = new JsonObject();
                }
            }
            catch (Exception ex)
            {
                _log.LogWarning(ex, "[Settings] load failed; starting empty (path={Path})", _path);
                _data = new JsonObject();
            }
        }
    }

    public T? Get<T>(string key, T? defaultValue = default)
    {
        lock (_gate)
        {
            if (!_data.TryGetPropertyValue(key, out var node) || node is null)
            {
                return defaultValue;
            }
            try
            {
                return node.Deserialize<T>(ReadOptions);
            }
            catch (Exception ex)
            {
                _log.LogWarning(ex, "[Settings] get<{T}>({Key}) deserialize failed", typeof(T).Name, key);
                return defaultValue;
            }
        }
    }

    public string? GetString(string key, string? defaultValue = null)
    {
        lock (_gate)
        {
            if (!_data.TryGetPropertyValue(key, out var node) || node is null)
            {
                return defaultValue;
            }
            return node is JsonValue v && v.TryGetValue<string>(out var s) ? s : node.ToString();
        }
    }

    public bool GetBool(string key, bool defaultValue = false)
    {
        lock (_gate)
        {
            if (_data.TryGetPropertyValue(key, out var node) && node is JsonValue v && v.TryGetValue<bool>(out var b))
            {
                return b;
            }
            return defaultValue;
        }
    }

    public int GetInt(string key, int defaultValue = 0)
    {
        lock (_gate)
        {
            if (_data.TryGetPropertyValue(key, out var node) && node is JsonValue v)
            {
                if (v.TryGetValue<int>(out var i)) return i;
                if (v.TryGetValue<double>(out var d)) return (int)d;
            }
            return defaultValue;
        }
    }

    public void Set<T>(string key, T value)
    {
        lock (_gate)
        {
            _data[key] = value is null ? null : JsonSerializer.SerializeToNode(value, WriteOptions);
        }
    }

    public bool Contains(string key)
    {
        lock (_gate)
        {
            return _data.ContainsKey(key);
        }
    }

    public IReadOnlyCollection<string> Keys()
    {
        lock (_gate)
        {
            return _data.Select(p => p.Key).ToArray();
        }
    }

    /// <summary>Snapshot of current data as a fresh, detached <see cref="JsonObject"/>.</summary>
    public JsonObject Snapshot()
    {
        lock (_gate)
        {
            return (JsonObject)_data.DeepClone();
        }
    }

    public string NormalizedUiMode => UiMode.Normalize(GetString(SettingsKeys.UiMode));

    public void Save()
    {
        lock (_gate)
        {
            foreach (var legacy in SettingsKeys.LegacyPrunedOnSave)
            {
                _data.Remove(legacy);
            }

            var directory = System.IO.Path.GetDirectoryName(_path);
            if (string.IsNullOrEmpty(directory))
            {
                directory = ".";
            }
            Directory.CreateDirectory(directory);

            string? tempPath = null;
            try
            {
                tempPath = System.IO.Path.Combine(directory, $"tmp{Guid.NewGuid():N}.tmp.json");
                var json = _data.ToJsonString(WriteOptions);
                using (var stream = new FileStream(tempPath, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                using (var writer = new StreamWriter(stream, new System.Text.UTF8Encoding(false)))
                {
                    writer.Write(json);
                    writer.Flush();
                    stream.Flush(true);
                }
                File.Move(tempPath, _path, overwrite: true);
                tempPath = null;
            }
            catch (Exception ex)
            {
                _log.LogError(ex, "[Settings] atomic save failed (path={Path}); falling back to direct write", _path);
                if (tempPath is not null)
                {
                    try { File.Delete(tempPath); } catch { /* swallow */ }
                }
                try
                {
                    File.WriteAllText(_path, _data.ToJsonString(WriteOptions), new System.Text.UTF8Encoding(false));
                }
                catch (Exception ex2)
                {
                    _log.LogError(ex2, "[Settings] direct write fallback also failed (path={Path})", _path);
                }
            }
        }
    }

    private void CleanupStaleTempFiles()
    {
        try
        {
            var directory = System.IO.Path.GetDirectoryName(_path);
            if (string.IsNullOrEmpty(directory) || !Directory.Exists(directory))
            {
                return;
            }
            foreach (var file in Directory.EnumerateFiles(directory, "tmp*.tmp.json"))
            {
                try { File.Delete(file); } catch { /* swallow */ }
            }
        }
        catch
        {
            // best-effort cleanup, never fatal
        }
    }
}
