using SaoAuto.Core.Updater;

namespace SaoAuto.Tests.Updater;

public class OrphanCleanupTests : IDisposable
{
    private readonly string _root;
    private readonly string _backup;

    public OrphanCleanupTests()
    {
        _root = Path.Combine(Path.GetTempPath(), "sao-orphan-" + Guid.NewGuid().ToString("N"));
        _backup = Path.Combine(_root, "_bak");
        Directory.CreateDirectory(Path.Combine(_root, "install"));
        Directory.CreateDirectory(_backup);
    }

    public void Dispose()
    {
        try { Directory.Delete(_root, recursive: true); } catch { }
    }

    private string Install => Path.Combine(_root, "install");

    private string Touch(params string[] segments)
    {
        var p = Path.Combine(new[] { Install }.Concat(segments).ToArray());
        var dir = Path.GetDirectoryName(p)!;
        Directory.CreateDirectory(dir);
        File.WriteAllText(p, "x");
        return p;
    }

    [Fact]
    public void NormalizeRelRejectsParentTraversal()
    {
        Assert.Throws<ArgumentException>(() => OrphanCleanup.NormalizeRel("../sneaky.dll"));
        Assert.Throws<ArgumentException>(() => OrphanCleanup.NormalizeRel("a/../b"));
    }

    [Fact]
    public void NormalizeRelRejectsAbsolutePaths()
    {
        Assert.Throws<ArgumentException>(() => OrphanCleanup.NormalizeRel(@"C:\abs.dll"));
        Assert.Throws<ArgumentException>(() => OrphanCleanup.NormalizeRel(""));
    }

    [Fact]
    public void NormalizeRelConvertsBackslashes()
    {
        Assert.Equal("a/b/c.dll", OrphanCleanup.NormalizeRel(@"a\b\c.dll"));
    }

    [Fact]
    public void SafeRemoveRefusesTopLevelExe()
    {
        Touch("foo.exe");
        var (ok, reason) = OrphanCleanup.SafeRemoveOrphan(Install, "foo.exe");
        Assert.False(ok);
        Assert.Contains("顶层 exe", reason);
        Assert.True(File.Exists(Path.Combine(Install, "foo.exe")));
    }

    [Fact]
    public void SafeRemoveRefusesProtectedNames()
    {
        Touch("update.exe");
        var (ok, _) = OrphanCleanup.SafeRemoveOrphan(Install, "update.exe");
        Assert.False(ok);
    }

    [Fact]
    public void SafeRemoveDeletesNestedFile()
    {
        var p = Touch("runtime", "old.dll");
        var (ok, reason) = OrphanCleanup.SafeRemoveOrphan(Install, "runtime/old.dll");
        Assert.True(ok);
        Assert.Equal("removed", reason);
        Assert.False(File.Exists(p));
    }

    [Fact]
    public void SafeRemoveTreatsMissingAsAlreadyClean()
    {
        var (ok, reason) = OrphanCleanup.SafeRemoveOrphan(Install, "runtime/never_existed.dll");
        Assert.True(ok);
        Assert.Equal("absent", reason);
    }

    [Fact]
    public void ApplyRemovedFilesBacksUpAndDeletes()
    {
        var p1 = Touch("data", "old1.bin");
        var p2 = Touch("data", "old2.bin");
        var result = OrphanCleanup.ApplyRemovedFiles(
            Install,
            new[] { "data/old1.bin", "data/old1.bin", "data/old2.bin" },  // includes dup
            _backup);
        Assert.Equal(2, result.RemovedRels.Count);
        Assert.Equal(2, result.Backups.Count);
        Assert.False(File.Exists(p1));
        Assert.False(File.Exists(p2));
        Assert.True(File.Exists(Path.Combine(_backup, "__removed__", "data", "old1.bin")));
    }

    [Fact]
    public void RestoreRemovedBackupsReinstatesFiles()
    {
        var p = Touch("data", "old1.bin");
        var result = OrphanCleanup.ApplyRemovedFiles(
            Install, new[] { "data/old1.bin" }, _backup);
        Assert.False(File.Exists(p));
        var n = OrphanCleanup.RestoreRemovedBackups(Install, result.Backups);
        Assert.Equal(1, n);
        Assert.True(File.Exists(p));
    }

    [Fact]
    public void ApplyRemovedFilesIgnoresBlankAndInvalidEntries()
    {
        Touch("data", "keep.bin");
        var result = OrphanCleanup.ApplyRemovedFiles(
            Install, new[] { "", "   ", "data/missing.bin" }, _backup);
        // missing file is "absent" → still counted as removed
        Assert.Single(result.RemovedRels);
        Assert.Empty(result.Backups);
    }
}
