using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.App.Menu;
using SaoAuto.App.Startup;
using SaoAuto.Core.Configuration;

namespace SaoAuto.App.Hotkeys;

/// <summary>
/// S162 — Composition helper for HideSeek toggle hotkey wiring.
/// Reads <see cref="SettingsKeys.HideSeekToggleHotkey"/> from
/// <see cref="SettingsManager"/>, parses through
/// <see cref="HotkeyBindingParser"/>, and constructs a
/// <see cref="HideSeekHotkeyBinding"/> against the supplied
/// <see cref="IGlobalHotkeyService"/>.
///
/// Returns the binding on success, or <c>null</c> when no setting
/// exists, the string is malformed, or registration throws (e.g.
/// another app already owns the key). All failure paths log a
/// warning and let app startup proceed.
/// </summary>
public static class HideSeekHotkeySetup
{
    public static HideSeekHotkeyBinding? TryWire(
        SettingsManager settings,
        IGlobalHotkeyService hotkeys,
        HideSeekLifecycle lifecycle,
        ILogger? logger = null)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        if (hotkeys is null) throw new ArgumentNullException(nameof(hotkeys));
        if (lifecycle is null) throw new ArgumentNullException(nameof(lifecycle));
        var log = logger ?? NullLogger.Instance;

        var raw = settings.GetString(SettingsKeys.HideSeekToggleHotkey);
        if (string.IsNullOrWhiteSpace(raw))
        {
            log.LogDebug("[HideSeekHotkeySetup] no toggle hotkey configured");
            return null;
        }
        if (!HotkeyBindingParser.TryParse(raw, out var binding))
        {
            log.LogWarning("[HideSeekHotkeySetup] could not parse hotkey '{Raw}'", raw);
            return null;
        }
        try
        {
            var hk = new HideSeekHotkeyBinding(hotkeys, lifecycle, binding);
            log.LogInformation("[HideSeekHotkeySetup] bound HideSeek toggle to '{Raw}'", raw);
            return hk;
        }
        catch (Exception ex)
        {
            log.LogWarning(ex, "[HideSeekHotkeySetup] failed to register '{Raw}' — proceeding without hotkey", raw);
            return null;
        }
    }
}
