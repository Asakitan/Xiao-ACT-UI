using System.Collections.Immutable;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

/// <summary>
/// S169 — Pin the new <see cref="WebBridgeLifecycle"/> attach hooks
/// (BuffMon, DPS, RecognitionStatus): each registers its command set
/// on the shared <see cref="BridgeRouter"/>, replaces a prior attach
/// idempotently, throws on dispose, and dispose unregisters them.
/// </summary>
public class Session169WebBridgeLifecycleAttachTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public Session169WebBridgeLifecycleAttachTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s169-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _path = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_path, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { }
    }

    private SettingsManager NewSettings() => new(_path);

    [Fact]
    public void AttachBuffMonRegistersCommands()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.AttachBuffMon(NewSettings());
        Assert.Contains(BridgeCommands.GetBuffMonEnabled, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.SetBuffMonEnabled, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void AttachBuffMonIsIdempotent()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.AttachBuffMon(NewSettings());
        lifecycle.AttachBuffMon(NewSettings());
        Assert.Contains(BridgeCommands.GetBuffMonEnabled, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void AttachDpsRegistersCommands()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.AttachDps(reset: () => { }, lastReport: () => null);
        Assert.Contains(BridgeCommands.ResetCombat, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.ShowLastDpsReport, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void AttachDpsAcceptsNullDelegates()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.AttachDps(reset: null, lastReport: null);
        Assert.Contains(BridgeCommands.ResetCombat, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void AttachDpsIsIdempotent()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.AttachDps(null, null);
        lifecycle.AttachDps(null, null);
        Assert.Contains(BridgeCommands.ResetCombat, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void AttachRecognitionRegistersAllThree()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.AttachRecognition(() => true);
        Assert.Contains(BridgeCommands.RecognitionStatus, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.StartRecognition, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.StopRecognition, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void AttachRecognitionIsIdempotent()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.AttachRecognition(() => true);
        lifecycle.AttachRecognition(() => false);
        Assert.Contains(BridgeCommands.RecognitionStatus, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void DisposeUnregistersAllAttachedBridges()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.AttachBuffMon(NewSettings());
        lifecycle.AttachDps(() => { }, () => null);
        lifecycle.AttachRecognition(() => true);
        lifecycle.Dispose();
        Assert.DoesNotContain(BridgeCommands.GetBuffMonEnabled, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.ResetCombat, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.RecognitionStatus, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void AttachAfterDisposeThrows()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.Dispose();
        Assert.Throws<ObjectDisposedException>(() => lifecycle.AttachBuffMon(NewSettings()));
        Assert.Throws<ObjectDisposedException>(() => lifecycle.AttachDps(null, null));
        Assert.Throws<ObjectDisposedException>(() => lifecycle.AttachRecognition(() => true));
    }

    [Fact]
    public void NullArgsThrow()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        Assert.Throws<ArgumentNullException>(() => lifecycle.AttachBuffMon(null!));
        Assert.Throws<ArgumentNullException>(() => lifecycle.AttachRecognition(null!));
    }
}
