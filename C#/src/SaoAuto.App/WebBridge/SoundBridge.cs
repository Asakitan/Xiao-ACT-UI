using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S193 — Routes the pywebview-shim's <c>sound.play</c> command into
/// the in-process <see cref="ISoundPlayer"/> + <see cref="SoundCatalog"/>.
///
/// Reply shape: <c>{played:bool, name:string, path?:string,
/// error?:string}</c>. <c>played:false</c> when the catalog can't
/// resolve the short name (typo, missing asset) or the player is
/// disabled. The bridge never throws; failures surface in the reply.
///
/// Idempotent. Dispose unregisters the command.
/// </summary>
public sealed class SoundBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly ISoundPlayer _player;
    private readonly SoundCatalog _catalog;
    private readonly SettingsManager? _settings;
    private readonly string[] _commands;
    private bool _disposed;

    public SoundBridge(
        BridgeRouter router,
        ISoundPlayer player,
        SoundCatalog catalog,
        SettingsManager? settings = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _player = player ?? throw new ArgumentNullException(nameof(player));
        _catalog = catalog ?? throw new ArgumentNullException(nameof(catalog));
        _settings = settings;
        _commands = new[]
        {
            BridgeCommands.PlaySound,
            BridgeCommands.SetSoundEnabled,
            BridgeCommands.SetSoundVolume,
        };
        ApplyPersistedSettings();
        router.Register(BridgeCommands.PlaySound, HandlePlay);
        router.Register(BridgeCommands.SetSoundEnabled, HandleSetEnabled);
        router.Register(BridgeCommands.SetSoundVolume, HandleSetVolume);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var command in _commands)
            _router.Unregister(command);
    }

    private JsonObject HandlePlay(JsonObject? payload)
    {
        var name = payload?["name"]?.GetValue<string>() ?? string.Empty;
        if (string.IsNullOrEmpty(name))
            return new JsonObject { ["played"] = false, ["error"] = "missing_name" };
        var path = _catalog.Resolve(name);
        if (string.IsNullOrEmpty(path))
            return new JsonObject { ["played"] = false, ["name"] = name, ["error"] = "unknown_clip" };
        if (!_player.Enabled)
            return new JsonObject { ["played"] = false, ["name"] = name, ["error"] = "disabled" };
        if (!System.IO.File.Exists(path))
            return new JsonObject { ["played"] = false, ["name"] = name, ["path"] = path, ["error"] = "missing_clip" };
        _player.Play(path);
        return new JsonObject { ["played"] = true, ["name"] = name, ["path"] = path };
    }

    private JsonObject HandleSetEnabled(JsonObject? payload)
    {
        bool enabled;
        try
        {
            enabled = payload?["enabled"]?.GetValue<bool>() ?? true;
        }
        catch
        {
            return new JsonObject { ["ok"] = false, ["error"] = "bad_payload" };
        }

        _player.Enabled = enabled;
        _settings?.Set(SettingsKeys.SoundEnabled, enabled);
        _settings?.Save();
        return new JsonObject
        {
            ["ok"] = true,
            ["enabled"] = enabled,
        };
    }

    private JsonObject HandleSetVolume(JsonObject? payload)
    {
        var node = payload?["volume"] ?? payload?["volume_pct"];
        if (!TryGetInt(node, out var rawVolume))
            return new JsonObject { ["ok"] = false, ["error"] = "bad_payload" };

        var volume = ClampVolume(rawVolume);
        _player.VolumePct = volume;
        _settings?.Set(SettingsKeys.SoundVolume, volume);
        _settings?.Save();
        return new JsonObject
        {
            ["ok"] = true,
            ["volume"] = volume,
            ["volume_pct"] = volume,
        };
    }

    private void ApplyPersistedSettings()
    {
        if (_settings is null) return;
        _player.Enabled = _settings.GetBool(SettingsKeys.SoundEnabled, _player.Enabled);
        _player.VolumePct = ClampVolume(_settings.GetInt(SettingsKeys.SoundVolume, _player.VolumePct));
    }

    private static int ClampVolume(int volume)
        => Math.Clamp(volume, 0, 100);

    private static bool TryGetInt(JsonNode? node, out int value)
    {
        value = 0;
        if (node is not JsonValue jsonValue) return false;
        if (jsonValue.TryGetValue<int>(out value)) return true;
        if (jsonValue.TryGetValue<double>(out var dbl))
        {
            value = (int)dbl;
            return true;
        }
        if (jsonValue.TryGetValue<string>(out var text)
            && int.TryParse(text, System.Globalization.NumberStyles.Integer,
                System.Globalization.CultureInfo.InvariantCulture, out value))
        {
            return true;
        }
        return false;
    }
}
