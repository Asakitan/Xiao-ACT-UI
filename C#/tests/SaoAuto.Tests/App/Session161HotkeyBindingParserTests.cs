using SaoAuto.App.Hotkeys;
using SaoAuto.App.Menu;

namespace SaoAuto.Tests.App;

/// <summary>
/// S161 — Pin <see cref="HotkeyBindingParser"/>: accepted modifier
/// vocabulary, single-key rules, F-key range, malformed input
/// returning <c>false</c> without throwing.
/// </summary>
public class Session161HotkeyBindingParserTests
{
    [Theory]
    [InlineData("Ctrl+Alt+H", HotkeyModifiers.Ctrl | HotkeyModifiers.Alt, 0x48)]
    [InlineData("ctrl+alt+h", HotkeyModifiers.Ctrl | HotkeyModifiers.Alt, 0x48)]
    [InlineData("control + shift + Z", HotkeyModifiers.Ctrl | HotkeyModifiers.Shift, 0x5A)]
    [InlineData("Win+F12", HotkeyModifiers.Win, 0x7B)]
    [InlineData("meta+5", HotkeyModifiers.Win, 0x35)]
    [InlineData("F1", HotkeyModifiers.None, 0x70)]
    [InlineData("Shift+F24", HotkeyModifiers.Shift, 0x87)]
    public void ParsesValidBindings(string text, HotkeyModifiers mods, int vk)
    {
        Assert.True(HotkeyBindingParser.TryParse(text, out var b));
        Assert.Equal(mods, b.Modifiers);
        Assert.Equal(vk, b.VirtualKey);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("+")]
    [InlineData("Ctrl+")]
    [InlineData("Ctrl+Alt")]            // no key token
    [InlineData("Ctrl+H+J")]            // two keys
    [InlineData("Ctrl+F25")]            // out of range
    [InlineData("Ctrl+F0")]             // out of range
    [InlineData("Ctrl+PrintScreen")]    // unsupported key name (named key vocab is fixed)
    [InlineData("Hyper+H")]             // unknown modifier
    [InlineData("Ctrl+!")]              // non-alnum key
    public void RejectsMalformedInput(string? text)
    {
        Assert.False(HotkeyBindingParser.TryParse(text, out var b));
        Assert.Equal(default, b);
    }
}
