using System.Globalization;
using SaoAuto.App.Menu;

namespace SaoAuto.App.Hotkeys;

/// <summary>
/// S161 — Parse user-facing hotkey strings (e.g. <c>"Ctrl+Alt+H"</c>,
/// <c>"Shift+F12"</c>) into a <see cref="HotkeyBinding"/> ready for
/// <see cref="IGlobalHotkeyService.Register"/>. Settings-driven hotkey
/// wiring (<see cref="HideSeekHotkeyBinding"/> and friends) reads a
/// string from <c>SettingsManager</c> and feeds it through this parser
/// so a config edit can change a binding without recompiling.
///
/// Format:
/// <list type="bullet">
///   <item>One or more modifier tokens separated by <c>+</c>:
///   <c>ctrl</c>/<c>control</c>, <c>alt</c>, <c>shift</c>,
///   <c>win</c>/<c>meta</c>. Case-insensitive. Whitespace stripped.</item>
///   <item>Exactly one final key token: a single letter (A–Z), a
///   digit (0–9), <c>F1</c>–<c>F24</c>, or a named key from the
///   table below (case-insensitive):
///   <c>esc</c>/<c>escape</c>, <c>tab</c>, <c>space</c>,
///   <c>enter</c>/<c>return</c>, <c>backspace</c>,
///   <c>ins</c>/<c>insert</c>, <c>del</c>/<c>delete</c>,
///   <c>home</c>, <c>end</c>,
///   <c>pageup</c>/<c>pgup</c>, <c>pagedown</c>/<c>pgdn</c>,
///   <c>up</c>, <c>down</c>, <c>left</c>, <c>right</c>.</item>
/// </list>
/// Returns <c>false</c> on any malformed input. Never throws.
/// </summary>
public static class HotkeyBindingParser
{
    private static readonly Dictionary<string, int> NamedKeys = new(StringComparer.Ordinal)
    {
        ["esc"] = 0x1B, ["escape"] = 0x1B,
        ["tab"] = 0x09,
        ["space"] = 0x20,
        ["enter"] = 0x0D, ["return"] = 0x0D,
        ["backspace"] = 0x08,
        ["ins"] = 0x2D, ["insert"] = 0x2D,
        ["del"] = 0x2E, ["delete"] = 0x2E,
        ["home"] = 0x24, ["end"] = 0x23,
        ["pageup"] = 0x21, ["pgup"] = 0x21,
        ["pagedown"] = 0x22, ["pgdn"] = 0x22,
        ["up"] = 0x26, ["down"] = 0x28, ["left"] = 0x25, ["right"] = 0x27,
    };

    public static bool TryParse(string? text, out HotkeyBinding binding)
    {
        binding = default;
        if (string.IsNullOrWhiteSpace(text)) return false;

        var parts = text.Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (parts.Length == 0) return false;

        var mods = HotkeyModifiers.None;
        int? vk = null;
        foreach (var raw in parts)
        {
            var token = raw.ToLowerInvariant();
            switch (token)
            {
                case "ctrl":
                case "control": mods |= HotkeyModifiers.Ctrl; continue;
                case "alt": mods |= HotkeyModifiers.Alt; continue;
                case "shift": mods |= HotkeyModifiers.Shift; continue;
                case "win":
                case "meta": mods |= HotkeyModifiers.Win; continue;
            }
            // Must be the key token; only one allowed.
            if (vk is not null) return false;
            if (!TryParseKey(token, out var parsedVk)) return false;
            vk = parsedVk;
        }
        if (vk is null) return false;
        binding = new HotkeyBinding(mods, vk.Value);
        return true;
    }

    private static bool TryParseKey(string token, out int vk)
    {
        vk = 0;
        if (token.Length == 0) return false;
        if (token.Length == 1)
        {
            var ch = char.ToUpperInvariant(token[0]);
            if (ch >= 'A' && ch <= 'Z') { vk = ch; return true; }
            if (ch >= '0' && ch <= '9') { vk = ch; return true; }
            return false;
        }
        if (token[0] == 'f' && token.Length <= 3
            && int.TryParse(token.AsSpan(1), NumberStyles.Integer, CultureInfo.InvariantCulture, out var n)
            && n >= 1 && n <= 24)
        {
            // VK_F1 = 0x70, VK_F24 = 0x87
            vk = 0x70 + (n - 1);
            return true;
        }
        if (NamedKeys.TryGetValue(token, out var named))
        {
            vk = named;
            return true;
        }
        return false;
    }
}
