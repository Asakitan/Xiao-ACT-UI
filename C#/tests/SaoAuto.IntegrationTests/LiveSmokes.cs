namespace SaoAuto.IntegrationTests;

/// <summary>
/// End-to-end smoke for the live SharpPcap → TcpReassembler →
/// PacketParser → GameStateManager pipeline. Requires Npcap on the
/// host (the SharpPcap driver fails to enumerate adapters otherwise).
/// </summary>
public class LivePacketSourceSmoke
{
    [Fact(Skip = LiveOnly.Reason)]
    public void EnumerateInterfaces_ReturnsAtLeastOne()
    {
        // Will become: var devs = SharpPcap.CaptureDeviceList.Instance;
        //              Assert.NotEmpty(devs);
        Assert.Fail("not implemented; live-only");
    }

    [Fact(Skip = LiveOnly.Reason)]
    public void StreamFromNamedAdapter_DeliversFirstPacket()
    {
        // Will become: open device by --adapter env, attach
        // SharpPcapPacketSource, await first ParserEvent within 5s.
        Assert.Fail("not implemented; live-only");
    }
}

/// <summary>
/// Live GDI / WGC frame-capture + RecognitionEngine smoke. Requires a
/// desktop session (xunit on a CI agent without one will fail without
/// the skip).
/// </summary>
public class LiveFrameCaptureSmoke
{
    [Fact(Skip = LiveOnly.Reason)]
    public void CaptureGameWindow_ReturnsValidBgraFrame()
    {
        Assert.Fail("not implemented; live-only");
    }

    [Fact(Skip = LiveOnly.Reason)]
    public void RecognitionEngine_ReportsKnownAnchors()
    {
        Assert.Fail("not implemented; live-only");
    }
}

/// <summary>
/// Live WebView2 + HTML load smoke. Requires the WebView2 runtime.
/// </summary>
public class LiveWebView2Smoke
{
    [Fact(Skip = LiveOnly.Reason)]
    public void HostLoads_MenuHtml_AndBridgeReceivesEvent()
    {
        Assert.Fail("not implemented; live-only");
    }
}

/// <summary>
/// Live Win32 hotkey loop + AutomationCore toggle smoke. Requires a
/// real message pump on a foreground thread.
/// </summary>
public class LiveHotkeySmoke
{
    [Fact(Skip = LiveOnly.Reason)]
    public void RegisterHotkey_TogglesAutomationCaptureFlag()
    {
        Assert.Fail("not implemented; live-only");
    }
}

/// <summary>
/// Live updater apply-and-rollback smoke against a throw-away
/// directory. Builds a mini package, runs SaoAuto.UpdateApply.exe
/// against it, asserts the staged file landed and then rolls back.
/// </summary>
public class LiveUpdaterSmoke
{
    [Fact(Skip = LiveOnly.Reason)]
    public void ApplyZip_PromotesFilesUnderInstallBase()
    {
        Assert.Fail("not implemented; live-only");
    }

    [Fact(Skip = LiveOnly.Reason)]
    public void ApplyZip_RollbackOnFailure_LeavesInstallUntouched()
    {
        Assert.Fail("not implemented; live-only");
    }
}
