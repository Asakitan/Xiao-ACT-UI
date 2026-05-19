using System.Globalization;
using System.Text;
using SaoAuto.App.Menu;

namespace SaoAuto.App.Hotkeys;

/// <summary>
/// S165 — Inverse of <see cref="HotkeyBindingParser"/>: render a
/// <see cref="HotkeyBinding"/> back to the canonical
/// <c>"Ctrl+Alt+H"</c> form. Used wherever a saved binding has to be
/// shown back to the user (settings screens, log lines, "press to
/// rebind" labels). Output uses Title-Case modifier order
/// <c>Ctrl, Alt, Shift, Win</c> and the same key vocabulary the
/// parser accepts, so <c>Parse(Format(b)) == b</c> for every binding
/// the parser can produce.
/// </summary>
public static class HotkeyBindingFormatter
{
    private static readonly (HotkeyModifiers Flag, string Label)[] ModifierOrder =
    {
        (HotkeyModifiers.Ctrl, "Ctrl"),
        (HotkeyModifiers.Alt, "Alt"),
        (HotkeyModifiers.Shift, "Shift"),
        (HotkeyModifiers.Win, "Win"),
    };

    private static readonly Dictionary<int, string> NamedKeyLabels = new()
    {
        [0x1B] = "Esc",
        [0x09] = "Tab",
        [0x20] = "Space",
        [0x0D] = "Enter",
        [0x08] = "Backspace",
        [0x2D] = "Insert",
        [0x2E] = "Delete",
        [0x24] = "Home",
        [0x23] = "End",
        [0x21] = "PageUp",
        [0x22] = "PageDown",
        [0x26] = "Up",
        [0x28] = "Down",
        [0x25] = "Left",
        [0x27] = "Right",
    };

    public static string Format(HotkeyBinding binding)
    {
        var sb = new StringBuilder();
        foreach (var (flag, label) in ModifierOrder)
        {
            if ((binding.Modifiers & flag) != 0)
            {
                if (sb.Length > 0) sb.Append('+');
                sb.Append(label);
            }
        }
        if (sb.Length > 0) sb.Append('+');
        sb.Append(KeyLabel(binding.VirtualKey));
        return sb.ToString();
    }

    private static string KeyLabel(int vk)
    {
        if (NamedKeyLabels.TryGetValue(vk, out var named)) return named;
        if (vk >= 0x70 && vk <= 0x87)
            return "F" + (vk - 0x70 + 1).ToString(CultureInfo.InvariantCulture);
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
            return ((char)vk).ToString();
        // Unknown VK — emit the hex so the round-trip at least reports something
        // diagnostic instead of silently producing an empty key segment.
        return "0x" + vk.ToString("X2", CultureInfo.InvariantCulture);
    }
}
