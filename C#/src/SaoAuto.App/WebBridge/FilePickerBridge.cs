using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S200 — Minimal native file-browser bridge for legacy menu pickers.
/// Python pywebview exposes <c>browse_dir</c> and
/// <c>start_auto_key_import_picker</c> / <c>start_boss_raid_import_picker</c>;
/// without these commands, WebView2 menus cannot open or navigate import
/// pickers.
/// </summary>
public sealed class FilePickerBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly Func<string> _rootProvider;
    private readonly SettingsManager? _settings;
    private readonly GameStateManager? _states;
    private readonly string[] _commands;
    private string _pendingConsumer = string.Empty;
    private bool _disposed;

    public FilePickerBridge(
        BridgeRouter router,
        Func<string>? rootProvider = null,
        SettingsManager? settings = null,
        GameStateManager? states = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _rootProvider = rootProvider ?? (() => AppContext.BaseDirectory);
        _settings = settings;
        _states = states;
        _commands = new[]
        {
            BridgeCommands.BrowseDir,
            BridgeCommands.SelectFile,
            BridgeCommands.SelectFolder,
            BridgeCommands.StartAutoKeyImportPicker,
            BridgeCommands.StartBossRaidImportPicker,
        };
        router.Register(BridgeCommands.BrowseDir, HandleBrowse);
        router.Register(BridgeCommands.SelectFile, HandleSelectFile);
        router.Register(BridgeCommands.SelectFolder, HandleSelectFolder);
        router.Register(BridgeCommands.StartAutoKeyImportPicker, HandleStartAutoKeyImportPicker);
        router.Register(BridgeCommands.StartBossRaidImportPicker, HandleStartBossRaidImportPicker);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var command in _commands)
            _router.Unregister(command);
    }

    private JsonObject HandleStartAutoKeyImportPicker(JsonObject? payload)
        => HandleStartImportPicker(payload, "auto_key");

    private JsonObject HandleStartBossRaidImportPicker(JsonObject? payload)
        => HandleStartImportPicker(payload, "boss_raid");

    private JsonObject HandleStartImportPicker(JsonObject? payload, string consumer)
    {
        var requested = ReadString(payload, "path");
        var root = string.IsNullOrWhiteSpace(requested) ? SafeRoot() : requested;
        _pendingConsumer = consumer;
        var browser = Browse(root);
        browser["mode"] = "file";
        if (browser["error"] is not null)
        {
            return new JsonObject
            {
                ["ok"] = false,
                ["message"] = browser["error"]!.GetValue<string>(),
                ["browser"] = browser,
            };
        }
        return new JsonObject
        {
            ["ok"] = true,
            ["browser"] = browser,
        };
    }

    private JsonObject HandleBrowse(JsonObject? payload)
    {
        var requested = ReadString(payload, "path");
        var path = string.IsNullOrWhiteSpace(requested) ? SafeRoot() : requested;
        return Browse(path);
    }

    private JsonObject HandleSelectFile(JsonObject? payload)
    {
        var path = ReadString(payload, "path");
        var consumer = NormalizeConsumer(ReadString(payload, "consumer"));
        if (string.IsNullOrEmpty(consumer))
            consumer = _pendingConsumer;
        if (string.IsNullOrWhiteSpace(path))
            return SelectError(path, "No file selected");
        if (_settings is null)
            return SelectError(path, "File import is not available in this WebView2 host");

        try
        {
            var fullPath = Path.GetFullPath(path);
            if (consumer == "auto_key")
            {
                var author = CurrentAuthor();
                var config = AutoKeyConfigLoader.Load(_settings, author);
                var profile = AutoKeyProfileStore.ImportProfileFromPath(fullPath, author);
                config = AutoKeyProfileStore.UpsertProfile(config, profile, activate: false);
                AutoKeyConfigLoader.Save(_settings, config);
                _settings.Save();
                _pendingConsumer = string.Empty;
                return new JsonObject
                {
                    ["ok"] = true,
                    ["path"] = fullPath,
                    ["consumer"] = consumer,
                    ["state"] = MenuStateBridge.BuildAutoKeyState(_settings, _states),
                };
            }
            if (consumer == "boss_raid")
            {
                using var authorDoc = JsonDocument.Parse(AuthorObject(CurrentAuthor()).ToJsonString());
                var authorElement = authorDoc.RootElement.Clone();
                var config = BossRaidConfigStore.Load(_settings, authorElement);
                var profile = BossRaidProfileIo.ImportProfileFromPath(fullPath, authorElement);
                config = BossRaidProfile.UpsertProfile(config, profile, activate: false);
                BossRaidConfigStore.Save(_settings, config);
                _pendingConsumer = string.Empty;
                return new JsonObject
                {
                    ["ok"] = true,
                    ["path"] = fullPath,
                    ["consumer"] = consumer,
                    ["state"] = MenuStateBridge.BuildBossRaidState(_settings, _states),
                };
            }
            return SelectError(path, "No file picker action pending");
        }
        catch (Exception ex)
        {
            return SelectError(path, ex.Message);
        }
    }

    private static JsonObject HandleSelectFolder(JsonObject? payload) => new()
    {
        ["ok"] = false,
        ["message"] = "Folder selection not used here",
        ["path"] = ReadString(payload, "path"),
    };

    private JsonObject Browse(string path)
    {
        var requested = path ?? string.Empty;
        try
        {
            var current = Path.GetFullPath(requested);
            var info = new DirectoryInfo(current);
            if (!info.Exists)
            {
                return Error(current, $"Directory not found: {current}");
            }

            var dirs = new JsonArray();
            foreach (var dir in info.EnumerateDirectories()
                         .Where(d => !d.Name.StartsWith(".", StringComparison.Ordinal))
                         .OrderBy(d => d.Name, StringComparer.OrdinalIgnoreCase))
            {
                dirs.Add(new JsonObject
                {
                    ["name"] = dir.Name,
                    ["path"] = dir.FullName,
                });
            }

            var files = new JsonArray();
            foreach (var file in info.EnumerateFiles()
                         .OrderBy(f => f.Name, StringComparer.OrdinalIgnoreCase))
            {
                files.Add(new JsonObject
                {
                    ["name"] = file.Name,
                    ["path"] = file.FullName,
                    ["size"] = file.Length,
                });
            }

            var parent = info.Parent?.FullName;
            return new JsonObject
            {
                ["current"] = info.FullName,
                ["parent"] = string.Equals(parent, info.FullName, StringComparison.OrdinalIgnoreCase) ? null : parent,
                ["dirs"] = dirs,
                ["files"] = files,
            };
        }
        catch (Exception ex)
        {
            return Error(requested, ex.Message);
        }
    }

    private string SafeRoot()
    {
        try
        {
            var root = _rootProvider();
            return string.IsNullOrWhiteSpace(root) ? AppContext.BaseDirectory : root;
        }
        catch
        {
            return AppContext.BaseDirectory;
        }
    }

    private static JsonObject Error(string current, string message) => new()
    {
        ["current"] = current ?? string.Empty,
        ["parent"] = null,
        ["dirs"] = new JsonArray(),
        ["files"] = new JsonArray(),
        ["error"] = message,
    };

    private static JsonObject SelectError(string path, string message) => new()
    {
        ["ok"] = false,
        ["message"] = message,
        ["path"] = path ?? string.Empty,
    };

    private AuthorSnapshot CurrentAuthor()
    {
        if (_states is null) return AuthorSnapshot.Empty;
        return AutoKeyConfigLoader.AuthorFromState(_states.Snapshot);
    }

    private static JsonObject AuthorObject(AuthorSnapshot author) => new()
    {
        ["player_uid"] = author.PlayerUid ?? string.Empty,
        ["player_name"] = author.PlayerName ?? string.Empty,
        ["profession_id"] = author.ProfessionId,
        ["profession_name"] = author.ProfessionName ?? string.Empty,
    };

    private static string NormalizeConsumer(string value)
    {
        var normalized = (value ?? string.Empty).Trim().ToLowerInvariant();
        return normalized switch
        {
            "autokey" or "auto_key" or "auto-key" => "auto_key",
            "bossraid" or "boss_raid" or "boss-raid" => "boss_raid",
            _ => string.Empty,
        };
    }

    private static string ReadString(JsonObject? payload, string key)
    {
        if (payload is null) return string.Empty;
        return payload.TryGetPropertyValue(key, out var node) && node is not null
            ? node.ToString()
            : string.Empty;
    }
}
