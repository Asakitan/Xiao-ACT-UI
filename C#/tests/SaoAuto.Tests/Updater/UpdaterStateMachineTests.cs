using SaoAuto.Core.Updater;

namespace SaoAuto.Tests.Updater;

public class UpdaterStateMachineTests
{
    [Fact]
    public void NewMachineIsIdle()
    {
        var sm = new UpdaterStateMachine();
        Assert.Equal(UpdaterStatus.Idle, sm.Snapshot.Status);
    }

    [Fact]
    public void CheckCompletedWithNewerVersionMarksAvailable()
    {
        var sm = new UpdaterStateMachine();
        var manifest = MakeManifest("5.0.1");
        sm.BeginCheck();
        sm.CheckCompleted(manifest, currentVersion: "5.0.0");
        Assert.Equal(UpdaterStatus.UpdateAvailable, sm.Snapshot.Status);
        Assert.Equal(manifest, sm.Snapshot.Latest);
    }

    [Fact]
    public void CheckCompletedWithOlderVersionMarksNoUpdate()
    {
        var sm = new UpdaterStateMachine();
        sm.BeginCheck();
        sm.CheckCompleted(MakeManifest("4.9.9"), currentVersion: "5.0.0");
        Assert.Equal(UpdaterStatus.NoUpdate, sm.Snapshot.Status);
    }

    [Fact]
    public void SuffixedVersionIsConsideredNewer()
    {
        var sm = new UpdaterStateMachine();
        sm.BeginCheck();
        sm.CheckCompleted(MakeManifest("5.0.0-a"), currentVersion: "5.0.0");
        Assert.Equal(UpdaterStatus.UpdateAvailable, sm.Snapshot.Status);
    }

    [Fact]
    public void DownloadProgressClampsToZeroOne()
    {
        var sm = new UpdaterStateMachine();
        sm.BeginCheck();
        sm.CheckCompleted(MakeManifest("5.0.1"), "5.0.0");
        sm.BeginDownload();
        sm.ReportProgress(0.5);
        Assert.Equal(0.5, sm.Snapshot.DownloadProgress, 4);
        sm.ReportProgress(1.5);
        Assert.Equal(1.0, sm.Snapshot.DownloadProgress, 4);
        sm.ReportProgress(-0.1);
        Assert.Equal(0.0, sm.Snapshot.DownloadProgress, 4);
    }

    [Fact]
    public void DownloadCompletedTransitionsToStagedReady()
    {
        var sm = new UpdaterStateMachine();
        sm.BeginCheck();
        sm.CheckCompleted(MakeManifest("5.0.1"), "5.0.0");
        sm.BeginDownload();
        sm.DownloadCompleted();
        Assert.Equal(UpdaterStatus.StagedReady, sm.Snapshot.Status);
    }

    [Fact]
    public void ApplyFailedRestoresStagedReadyForRetry()
    {
        var sm = new UpdaterStateMachine();
        sm.BeginCheck();
        sm.CheckCompleted(MakeManifest("5.0.1"), "5.0.0");
        sm.BeginDownload();
        sm.DownloadCompleted();
        sm.BeginApply();
        sm.ApplyFailed("helper exited 1");
        Assert.Equal(UpdaterStatus.StagedReady, sm.Snapshot.Status);
        Assert.Equal("helper exited 1", sm.Snapshot.Error);
    }

    [Fact]
    public void DownloadFailedReturnsToUpdateAvailable()
    {
        var sm = new UpdaterStateMachine();
        sm.BeginCheck();
        sm.CheckCompleted(MakeManifest("5.0.1"), "5.0.0");
        sm.BeginDownload();
        sm.DownloadFailed("network");
        Assert.Equal(UpdaterStatus.UpdateAvailable, sm.Snapshot.Status);
        Assert.Equal("network", sm.Snapshot.Error);
    }

    [Fact]
    public void StateChangedFiresPerTransition()
    {
        var sm = new UpdaterStateMachine();
        var states = new List<UpdaterState>();
        sm.StateChanged += states.Add;
        sm.BeginCheck();
        sm.CheckCompleted(MakeManifest("5.0.1"), "5.0.0");
        Assert.Equal(2, states.Count);
        Assert.Equal(UpdaterStatus.Checking, states[0].Status);
        Assert.Equal(UpdaterStatus.UpdateAvailable, states[1].Status);
    }

    private static UpdateManifest MakeManifest(string version) => new(
        Version: version,
        Channel: "stable",
        Target: "windows-x64",
        PackageUrl: "http://example/pkg.zip",
        PackageSha256: new string('0', 64),
        PackageSize: 100,
        Kind: UpdatePackageKind.Full,
        Notes: null,
        PublishedAt: DateTimeOffset.UtcNow);
}

public class UpdateExePromoterTests
{
    [Fact]
    public void NonMainHostNeverPromotes()
    {
        var ctx = new UpdateExeContext(IsMainAppHost: false, HasUpdateExe: true, HasUpdateExeNew: true,
            HasRuntimeNestedUpdateExe: false, UpdateExeIdenticalToNew: false);
        Assert.Equal(UpdatePromotionAction.Skip, UpdateExePromoter.DecideUpdateExeNew(ctx));
        Assert.Equal(UpdatePromotionAction.Skip, UpdateExePromoter.DecideRuntimeUpdateExe(ctx));
    }

    [Fact]
    public void IdenticalStagedFileIsDropped()
    {
        var ctx = new UpdateExeContext(true, HasUpdateExe: true, HasUpdateExeNew: true,
            HasRuntimeNestedUpdateExe: false, UpdateExeIdenticalToNew: true);
        Assert.Equal(UpdatePromotionAction.DropStaged, UpdateExePromoter.DecideUpdateExeNew(ctx));
    }

    [Fact]
    public void MissingTopLevelTriggersPromotion()
    {
        var ctx = new UpdateExeContext(true, HasUpdateExe: false, HasUpdateExeNew: true,
            HasRuntimeNestedUpdateExe: false, UpdateExeIdenticalToNew: false);
        Assert.Equal(UpdatePromotionAction.PromoteAtomicReplace, UpdateExePromoter.DecideUpdateExeNew(ctx));
    }

    [Fact]
    public void RuntimeNestedDroppedWhenTopLevelExists()
    {
        var ctx = new UpdateExeContext(true, HasUpdateExe: true, HasUpdateExeNew: false,
            HasRuntimeNestedUpdateExe: true, UpdateExeIdenticalToNew: false);
        Assert.Equal(UpdatePromotionAction.DropNested, UpdateExePromoter.DecideRuntimeUpdateExe(ctx));
    }

    [Fact]
    public void RuntimeNestedPromotedWhenTopLevelMissing()
    {
        var ctx = new UpdateExeContext(true, HasUpdateExe: false, HasUpdateExeNew: false,
            HasRuntimeNestedUpdateExe: true, UpdateExeIdenticalToNew: false);
        Assert.Equal(UpdatePromotionAction.PromoteAtomicReplace, UpdateExePromoter.DecideRuntimeUpdateExe(ctx));
    }
}
