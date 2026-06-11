using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.App;

public class Session200FilePickerBridgeTests : IDisposable
{
    private readonly string _workDir;

    public Session200FilePickerBridgeTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s200-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        Directory.CreateDirectory(Path.Combine(_workDir, "Beta"));
        Directory.CreateDirectory(Path.Combine(_workDir, "alpha"));
        Directory.CreateDirectory(Path.Combine(_workDir, ".hidden"));
        File.WriteAllText(Path.Combine(_workDir, "zeta.json"), "{}");
        File.WriteAllText(Path.Combine(_workDir, "Alpha.json"), "{}{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { }
    }

    private static BridgeMessage Cmd(string name, JsonObject? payload = null)
        => new(BridgeMessage.TypeCommand, name, payload);

    [Fact]
    public void BrowseDirListsSortedVisibleDirsAndFiles()
    {
        var router = new BridgeRouter();
        using var bridge = new FilePickerBridge(router, () => _workDir);

        var reply = router.Dispatch(Cmd(BridgeCommands.BrowseDir, new JsonObject { ["path"] = _workDir }));
        var payload = reply!.Payload!;

        Assert.Equal(Path.GetFullPath(_workDir), payload["current"]!.GetValue<string>());
        Assert.NotNull(payload["parent"]);

        var dirs = payload["dirs"]!.AsArray();
        Assert.Equal(2, dirs.Count);
        Assert.Equal("alpha", dirs[0]!["name"]!.GetValue<string>());
        Assert.Equal("Beta", dirs[1]!["name"]!.GetValue<string>());

        var files = payload["files"]!.AsArray();
        Assert.Equal(2, files.Count);
        Assert.Equal("Alpha.json", files[0]!["name"]!.GetValue<string>());
        Assert.Equal(4L, files[0]!["size"]!.GetValue<long>());
        Assert.Equal("zeta.json", files[1]!["name"]!.GetValue<string>());
    }

    [Fact]
    public void StartAutoKeyImportPickerReturnsFileModeBrowser()
    {
        var router = new BridgeRouter();
        using var bridge = new FilePickerBridge(router, () => _workDir);

        var reply = router.Dispatch(Cmd(BridgeCommands.StartAutoKeyImportPicker));
        var payload = reply!.Payload!;
        var browser = payload["browser"]!.AsObject();

        Assert.True(payload["ok"]!.GetValue<bool>());
        Assert.Equal("file", browser["mode"]!.GetValue<string>());
        Assert.Equal(Path.GetFullPath(_workDir), browser["current"]!.GetValue<string>());
        Assert.NotEmpty(browser["files"]!.AsArray());
    }

    [Fact]
    public void MissingPathReturnsPickerConsumableEmptyBrowser()
    {
        var router = new BridgeRouter();
        using var bridge = new FilePickerBridge(router, () => _workDir);
        var missing = Path.Combine(_workDir, "missing");

        var reply = router.Dispatch(Cmd(BridgeCommands.BrowseDir, new JsonObject { ["path"] = missing }));
        var payload = reply!.Payload!;

        Assert.Equal(Path.GetFullPath(missing), payload["current"]!.GetValue<string>());
        Assert.NotNull(payload["error"]);
        Assert.Empty(payload["dirs"]!.AsArray());
        Assert.Empty(payload["files"]!.AsArray());
    }

    [Fact]
    public void DisposeUnregistersCommands()
    {
        var router = new BridgeRouter();
        var bridge = new FilePickerBridge(router, () => _workDir);

        Assert.Contains(BridgeCommands.BrowseDir, router.RegisteredCommands);
        Assert.Contains(BridgeCommands.StartAutoKeyImportPicker, router.RegisteredCommands);

        bridge.Dispose();

        Assert.DoesNotContain(BridgeCommands.BrowseDir, router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.StartAutoKeyImportPicker, router.RegisteredCommands);
    }

    [Fact]
    public void LifecycleAttachRegistersAndDisposesFilePickerCommands()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());

        lifecycle.AttachFilePicker(() => _workDir);
        lifecycle.AttachFilePicker(() => _workDir);

        Assert.Contains(BridgeCommands.BrowseDir, lifecycle.Router.RegisteredCommands);
        Assert.Contains(BridgeCommands.StartAutoKeyImportPicker, lifecycle.Router.RegisteredCommands);

        var reply = lifecycle.Router.Dispatch(Cmd(BridgeCommands.StartAutoKeyImportPicker));
        Assert.True(reply!.Payload!["ok"]!.GetValue<bool>());

        lifecycle.Dispose();

        Assert.DoesNotContain(BridgeCommands.BrowseDir, lifecycle.Router.RegisteredCommands);
        Assert.DoesNotContain(BridgeCommands.StartAutoKeyImportPicker, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void LifecycleAttachAfterDisposeThrows()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.Dispose();

        Assert.Throws<ObjectDisposedException>(() => lifecycle.AttachFilePicker(() => _workDir));
    }
}
