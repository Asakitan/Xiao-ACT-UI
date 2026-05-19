using SaoAuto.App.Hotkeys;
using SaoAuto.App.Menu;

namespace SaoAuto.Tests.App;

/// <summary>
/// S165 — Pin <see cref="HotkeyBindingFormatter.Format"/>: canonical
/// modifier order, named-key labels, F-key labels, letter / digit
/// labels, and that
/// <see cref="HotkeyBindingParser.TryParse"/>(<see cref="HotkeyBindingFormatter.Format"/>(b)) == b
/// for every binding the parser can produce.
/// </summary>
public class Session165HotkeyBindingFormatterTests
{
    [Theory]
    [InlineData(HotkeyModifiers.Ctrl | HotkeyModifiers.Alt, 0x48, "Ctrl+Alt+H")]
    [InlineData(HotkeyModifiers.Ctrl | HotkeyModifiers.Shift, 0x5A, "Ctrl+Shift+Z")]
    [InlineData(HotkeyModifiers.Win, 0x7B, "Win+F12")]
    [InlineData(HotkeyModifiers.None, 0x70, "F1")]
    [InlineData(HotkeyModifiers.Shift, 0x87, "Shift+F24")]
    [InlineData(HotkeyModifiers.None, 0x1B, "Esc")]
    [InlineData(HotkeyModifiers.Ctrl | HotkeyModifiers.Alt, 0x2E, "Ctrl+Alt+Delete")]
    [InlineData(HotkeyModifiers.Win, 0x26, "Win+Up")]
    [InlineData(HotkeyModifiers.None, 0x35, "5")]
    public void FormatsExpectedString(HotkeyModifiers mods, int vk, string expected)
    {
        Assert.Equal(expected, HotkeyBindingFormatter.Format(new HotkeyBinding(mods, vk)));
    }

    [Fact]
    public void ModifierOrderIsCtrlAltShiftWinRegardlessOfFlagOrder()
    {
        // Flags happen to be ordered Alt=1, Ctrl=2, Shift=4, Win=8 — verify the
        // formatter overrides bitwise iteration with the user-visible order.
        var b = new HotkeyBinding(
            HotkeyModifiers.Win | HotkeyModifiers.Shift | HotkeyModifiers.Alt | HotkeyModifiers.Ctrl,
            0x48);
        Assert.Equal("Ctrl+Alt+Shift+Win+H", HotkeyBindingFormatter.Format(b));
    }

    [Fact]
    public void UnknownVkFallsBackToHex()
    {
        // VK_OEM_PLUS (0xBB) isn't in the parser's vocab.
        var s = HotkeyBindingFormatter.Format(new HotkeyBinding(HotkeyModifiers.None, 0xBB));
        Assert.Equal("0xBB", s);
    }

    [Theory]
    [InlineData("Ctrl+Alt+H")]
    [InlineData("Shift+F24")]
    [InlineData("Win+F12")]
    [InlineData("F1")]
    [InlineData("Esc")]
    [InlineData("Ctrl+Alt+Delete")]
    [InlineData("Win+Up")]
    [InlineData("Alt+PageDown")]
    [InlineData("Ctrl+Shift+Tab")]
    [InlineData("5")]
    [InlineData("Z")]
    public void ParseFormatRoundTrips(string input)
    {
        Assert.True(HotkeyBindingParser.TryParse(input, out var b));
        var formatted = HotkeyBindingFormatter.Format(b);
        Assert.True(HotkeyBindingParser.TryParse(formatted, out var b2));
        Assert.Equal(b, b2);
    }
}
