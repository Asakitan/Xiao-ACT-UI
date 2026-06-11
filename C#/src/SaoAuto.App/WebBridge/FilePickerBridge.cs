using System.IO;
using System.Text.Json.Nodes;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S200 — Minimal native file-browser bridge for legacy menu pickers.
/// Python pywebview exposes <c>browse_dir</c> and
/// <c>start_auto_key_import_picker</c>; without these commands, WebView2
/// menus cannot open or navigate the AutoKey import picker.
/// </summary>
public sealed class FilePickerBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly Func<string> _rootProvider;
    private readonly string[] _commands;
    private bool _disposed;

    public FilePickerBridge(BridgeRouter router, Func<string>? rootProvider = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _rootProvider = rootProvider ?? (() => AppContext.BaseDirectory);
        _commands = new[]
        {
            BridgeCommands.BrowseDir,
            BridgeCommands.SelectFolder,
            BridgeCommands.StartAutoKeyImportPicker,
        };
        router.Register(BridgeCommands.BrowseDir, HandleBrowse);
        router.Register(BridgeCommands.SelectFolder, HandleSelectFolder);
        router.Register(BridgeCommands.StartAutoKeyImportPicker, HandleStartAutoKeyImportPicker);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var command in _commands)
            _router.Unregister(command);
    }

    private JsonObject HandleStartAutoKeyImportPicker(JsonObject? payload)
    {
        var requested = ReadString(payload, "path");
        var root = string.IsNullOrWhiteSpace(requested) ? SafeRoot() : requested;
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

    private static string ReadString(JsonObject? payload, string key)
    {
        if (payload is null) return string.Empty;
        return payload.TryGetPropertyValue(key, out var node) && node is not null
            ? node.ToString()
            : string.Empty;
    }
}
