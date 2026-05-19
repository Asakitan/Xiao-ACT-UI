using SaoAuto.App.Menu;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Updater;

namespace SaoAuto.Tests.Live;

/// <summary>
/// Smoke tests for the live Win32-backed services. They never touch a
/// real game window or send real input — they verify constructors,
/// disposal idempotency, and basic shape so the live wiring sessions
/// have a regression net.
/// </summary>
public class Win32SmokeTests
{
    [Fact]
    public void Win32GlobalHotkeyServiceDisposeIsIdempotent()
    {
        var service = new Win32GlobalHotkeyService();
        service.Dispose();
        service.Dispose();
    }

    [Fact]
    public void Win32GlobalHotkeyServiceProcessMessageRejectsUnregisteredId()
    {
        using var service = new Win32GlobalHotkeyService();
        var triggered = 0;
        service.Triggered += _ => triggered++;
        // WM_HOTKEY = 0x0312, but no binding registered → ignored.
        Assert.False(service.ProcessMessage(0x0312, new IntPtr(99999)));
        Assert.Equal(0, triggered);
    }

    [Fact]
    public void Win32GlobalHotkeyServiceUnknownMessageReturnsFalse()
    {
        using var service = new Win32GlobalHotkeyService();
        Assert.False(service.ProcessMessage(0x0000, IntPtr.Zero));
    }

    [Fact]
    public void Win32SendInputCanBeConstructed()
    {
        // Smoke: verifies the type is reachable. Calling PressKey would
        // actually send keystrokes to the foreground window, which is not
        // safe in CI.
        var sender = new Win32SendInput();
        Assert.NotNull(sender);
    }

    [Fact]
    public void HttpUpdateClientCanBeConstructed()
    {
        var client = new HttpUpdateClient();
        Assert.NotNull(client);
    }

    [Fact]
    public async Task HttpUpdateClientCheckLatestReturnsNullOnUnreachableHost()
    {
        var client = new HttpUpdateClient();
        var result = await client.CheckLatestAsync(
            "http://127.0.0.1:1", "stable", "windows-x64");
        Assert.Null(result);
    }
}
