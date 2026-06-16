using System.IO.Compression;
using SaoAuto.Core.Updater;

namespace SaoAuto.Tests.Updater;

public class ApplyEngineTests : IDisposable
{
    private readonly string _tmp;

    public ApplyEngineTests()
    {
        _tmp = Path.Combine(Path.GetTempPath(), $"sao_apply_{Guid.NewGuid():N}");
        Directory.CreateDirectory(_tmp);
    }

    public void Dispose()
    {
        try { Directory.Delete(_tmp, recursive: true); } catch { }
    }

    private string MakeZip(IEnumerable<(string rel, string content)> files, bool includeTopExe = false)
    {
        var zip = Path.Combine(_tmp, $"pkg_{Guid.NewGuid():N}.zip");
        using var fs = File.Create(zip);
        using var zw = new ZipArchive(fs, ZipArchiveMode.Create);
        foreach (var (rel, content) in files)
        {
            var entry = zw.CreateEntry(rel);
            using var s = entry.Open();
            using var w = new StreamWriter(s);
            w.Write(content);
        }
        if (includeTopExe)
        {
            var entry = zw.CreateEntry("ignored.exe");
            using var s = entry.Open();
            using var w = new StreamWriter(s);
            w.Write("EXE");
        }
        return zip;
    }

    [Fact]
    public void ApplyZip_PromotesAllFilesAndDropsBak()
    {
        var pkg = MakeZip(new[]
        {
            ("data/a.txt", "AAA"),
            ("data/b.txt", "BBB"),
            ("README", "rd"),
        });
        var dest = Path.Combine(_tmp, "install");
        var engine = new ApplyEngine();
        var outcome = engine.ApplyZip(new ApplyEngine.ApplyOptions(pkg, dest));
        Assert.True(outcome.Success);
        Assert.Equal(3, outcome.FilesReplaced);
        Assert.Equal("AAA", File.ReadAllText(Path.Combine(dest, "data", "a.txt")));
        Assert.Equal("BBB", File.ReadAllText(Path.Combine(dest, "data", "b.txt")));
        Assert.Equal("rd", File.ReadAllText(Path.Combine(dest, "README")));
        // .bak shadows cleaned
        Assert.False(File.Exists(Path.Combine(dest, "data", "a.txt.bak")));
        Assert.False(File.Exists(Path.Combine(dest, "data", "a.txt.new")));
    }

    [Fact]
    public void ApplyZip_RollsBackOnReplaceFailure()
    {
        var pkg = MakeZip(new[]
        {
            ("ok1.txt", "ok1-new"),
            ("ok2.txt", "ok2-new"),
            ("bad.txt", "bad-new"),
        });
        var dest = Path.Combine(_tmp, "install");
        Directory.CreateDirectory(dest);
        File.WriteAllText(Path.Combine(dest, "ok1.txt"), "ok1-old");
        File.WriteAllText(Path.Combine(dest, "ok2.txt"), "ok2-old");
        File.WriteAllText(Path.Combine(dest, "bad.txt"), "bad-old");

        // Inject a replacer that fails on bad.txt.
        bool Replace(string src, string dst, string bak)
        {
            if (Path.GetFileName(dst) == "bad.txt") return false;
            try
            {
                if (File.Exists(dst)) File.Replace(src, dst, bak, ignoreMetadataErrors: true);
                else File.Move(src, dst);
                return true;
            }
            catch { return false; }
        }
        var engine = new ApplyEngine(replaceWithRetry: Replace);
        var outcome = engine.ApplyZip(new ApplyEngine.ApplyOptions(pkg, dest));
        Assert.False(outcome.Success);
        Assert.Equal(2, outcome.FilesReplaced);   // promoted before failure
        Assert.Equal(2, outcome.FilesRolledBack); // both restored
        Assert.Equal("ok1-old", File.ReadAllText(Path.Combine(dest, "ok1.txt")));
        Assert.Equal("ok2-old", File.ReadAllText(Path.Combine(dest, "ok2.txt")));
        Assert.Equal("bad-old", File.ReadAllText(Path.Combine(dest, "bad.txt")));
    }

    [Fact]
    public void ApplyZip_SkipsTopLevelExeUnlessAllowed()
    {
        var pkg = MakeZip(new[] { ("payload.txt", "p") }, includeTopExe: true);
        var dest = Path.Combine(_tmp, "install");
        var engine = new ApplyEngine();
        var outcome = engine.ApplyZip(new ApplyEngine.ApplyOptions(pkg, dest));
        Assert.True(outcome.Success);
        Assert.Equal(1, outcome.FilesReplaced);
        Assert.False(File.Exists(Path.Combine(dest, "ignored.exe")));

        var outcome2 = engine.ApplyZip(new ApplyEngine.ApplyOptions(pkg, dest, AllowTopLevelExe: true));
        Assert.True(outcome2.Success);
        Assert.True(File.Exists(Path.Combine(dest, "ignored.exe")));
    }

    [Fact]
    public void Sha256Hex_StableForFixedContent()
    {
        var path = Path.Combine(_tmp, "h.bin");
        File.WriteAllBytes(path, new byte[] { 0xDE, 0xAD, 0xBE, 0xEF });
        // sha256(deadbeef) = 5f78c33274e43fa9de5659265c1d917e25c03722dcb0b8d27db8d5feaa813953
        Assert.Equal("5f78c33274e43fa9de5659265c1d917e25c03722dcb0b8d27db8d5feaa813953",
            ApplyEngine.Sha256Hex(path));
    }

    [Fact]
    public void PendingApplyMeta_RoundTripsJson()
    {
        var meta = new PendingApplyMeta("p.zip", "C:/install", 1234, "C:/install/run.exe", "stable", "5.0.0");
        var json = meta.ToJson();
        var back = PendingApplyMeta.FromJson(json);
        Assert.Equal(meta, back);
    }
}
