using SaoAuto.App.Hotkeys;
using SaoAuto.App.Menu;

namespace SaoAuto.Tests.App;

/// <summary>
/// S164 — Extend <see cref="HotkeyBindingParser"/> with named-key
/// vocabulary (Esc, Tab, Space, Enter, Backspace, Insert, Delete,
/// Home, End, PageUp/Down, arrow keys). Pins the VK mapping for each
/// alias so settings strings written on one machine stay parseable on
/// another. Also confirms named keys compose with the existing
/// modifier vocabulary and that aliases (Esc / Escape, PgUp / PageUp)
/// resolve to the same VK.
/// </summary>
public class Session164HotkeyBindingNamedKeysTests
{
    [Theory]
    [InlineData("Esc", 0x1B)]
    [InlineData("escape", 0x1B)]
    [InlineData("Tab", 0x09)]
    [InlineData("Space", 0x20)]
    [InlineData("Enter", 0x0D)]
    [InlineData("Return", 0x0D)]
    [InlineData("Backspace", 0x08)]
    [InlineData("Ins", 0x2D)]
    [InlineData("Insert", 0x2D)]
    [InlineData("Del", 0x2E)]
    [InlineData("Delete", 0x2E)]
    [InlineData("Home", 0x24)]
    [InlineData("End", 0x23)]
    [InlineData("PageUp", 0x21)]
    [InlineData("PgUp", 0x21)]
    [InlineData("PageDown", 0x22)]
    [InlineData("PgDn", 0x22)]
    [InlineData("Up", 0x26)]
    [InlineData("Down", 0x28)]
    [InlineData("Left", 0x25)]
    [InlineData("Right", 0x27)]
    public void NamedKeysParseStandaloneWithExpectedVk(string text, int vk)
    {
        Assert.True(HotkeyBindingParser.TryParse(text, out var b));
        Assert.Equal(HotkeyModifiers.None, b.Modifiers);
        Assert.Equal(vk, b.VirtualKey);
    }

    [Theory]
    [InlineData("Ctrl+Esc", HotkeyModifiers.Ctrl, 0x1B)]
    [InlineData("Shift+Tab", HotkeyModifiers.Shift, 0x09)]
    [InlineData("Ctrl+Alt+Delete", HotkeyModifiers.Ctrl | HotkeyModifiers.Alt, 0x2E)]
    [InlineData("Win + Up", HotkeyModifiers.Win, 0x26)]
    [InlineData("alt+pagedown", HotkeyModifiers.Alt, 0x22)]
    public void NamedKeysComposeWithModifiers(string text, HotkeyModifiers mods, int vk)
    {
        Assert.True(HotkeyBindingParser.TryParse(text, out var b));
        Assert.Equal(mods, b.Modifiers);
        Assert.Equal(vk, b.VirtualKey);
    }

    [Fact]
    public void TwoNamedKeysStillRejected()
    {
        Assert.False(HotkeyBindingParser.TryParse("Ctrl+Esc+Tab", out _));
    }
}
