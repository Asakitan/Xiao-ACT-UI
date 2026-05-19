using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.App.Hotkeys;
using SaoAuto.App.Menu;
using SaoAuto.App.Startup;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Tests.App;

/// <summary>
/// S162 — Pin <see cref="HideSeekHotkeySetup.TryWire"/>: returns a
/// binding when settings hold a valid hotkey string, returns null on
/// missing / malformed / registration-failure paths without throwing,
/// and the resulting binding actually flips the lifecycle's suspended
/// state on trigger.
/// </summary>
public class Session162HideSeekHotkeySetupTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public Session162HideSeekHotkeySetupTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s162-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _path = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_path, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    private sealed class FakeHotkeys : IGlobalHotkeyService
    {
        public List<int> Registered { get; } = new();
        public bool ThrowOnRegister { get; set; }
        public event Action<int>? Triggered;
        public int Register(HotkeyBinding binding)
        {
            if (ThrowOnRegister) throw new InvalidOperationException("conflict");
            var id = 1000 + Registered.Count;
            Registered.Add(id);
            return id;
        }
        public void Unregister(int id) { }
        public void Dispose() { }
        public void Fire(int id) => Triggered?.Invoke(id);
    }

    private HideSeekLifecycle NewInactiveLifecycle()
        => HideSeekLifecycle.Start(
            () => throw new InvalidOperationException("noop"),
            NullLogger.Instance, CancellationToken.None);

    private SettingsManager NewSettings(string? hotkey = null)
    {
        var sm = new SettingsManager(_path);
        if (hotkey is not null)
        {
            sm.Set(SettingsKeys.HideSeekToggleHotkey, hotkey);
            sm.Save();
        }
        return sm;
    }

    [Fact]
    public void NoSettingReturnsNull()
    {
        using var lc = NewInactiveLifecycle();
        var sm = NewSettings();
        var hk = new FakeHotkeys();
        var bind = HideSeekHotkeySetup.TryWire(sm, hk, lc);
        Assert.Null(bind);
        Assert.Empty(hk.Registered);
    }

    [Fact]
    public void MalformedSettingReturnsNull()
    {
        using var lc = NewInactiveLifecycle();
        var sm = NewSettings("Hyper+Q");
        var hk = new FakeHotkeys();
        var bind = HideSeekHotkeySetup.TryWire(sm, hk, lc);
        Assert.Null(bind);
        Assert.Empty(hk.Registered);
    }

    [Fact]
    public void RegistrationFailureReturnsNull()
    {
        using var lc = NewInactiveLifecycle();
        var sm = NewSettings("Ctrl+Alt+H");
        var hk = new FakeHotkeys { ThrowOnRegister = true };
        var bind = HideSeekHotkeySetup.TryWire(sm, hk, lc);
        Assert.Null(bind);
    }

    [Fact]
    public void ValidSettingProducesWorkingBinding()
    {
        using var lc = NewInactiveLifecycle();
        var sm = NewSettings("Ctrl+Alt+H");
        var hk = new FakeHotkeys();
        using var bind = HideSeekHotkeySetup.TryWire(sm, hk, lc);
        Assert.NotNull(bind);
        Assert.Single(hk.Registered);

        // Inactive lifecycle stays not-suspended after firing.
        hk.Fire(hk.Registered[0]);
        Assert.False(lc.IsSuspended);
    }

    [Fact]
    public void NullArgsThrow()
    {
        using var lc = NewInactiveLifecycle();
        var sm = NewSettings();
        var hk = new FakeHotkeys();
        Assert.Throws<ArgumentNullException>(() => HideSeekHotkeySetup.TryWire(null!, hk, lc));
        Assert.Throws<ArgumentNullException>(() => HideSeekHotkeySetup.TryWire(sm, null!, lc));
        Assert.Throws<ArgumentNullException>(() => HideSeekHotkeySetup.TryWire(sm, hk, null!));
    }
}
