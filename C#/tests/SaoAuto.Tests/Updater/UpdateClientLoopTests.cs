using System.Net;
using System.Text;
using System.Text.Json;
using SaoAuto.Core.Updater;

namespace SaoAuto.Tests.Updater;

public class UpdateClientLoopTests
{
    [Fact]
    public async Task TickFetchesManifestStagesDownloadAndDoesNotApplyByDefault()
    {
        using var ws = new TempWorkspace();
        var manifest = NewManifest("2.0.0");
        var zipBytes = Encoding.UTF8.GetBytes("payload");
        var sha = Sha256Hex(zipBytes);
        var (loop, machine, _) = BuildLoop(ws, manifest with { PackageSha256 = sha }, zipBytes);

        await loop.TickAsync();

        Assert.Equal(UpdaterStatus.StagedReady, machine.Snapshot.Status);
        Assert.Equal("2.0.0", loop.StagedVersion);
        Assert.True(File.Exists(loop.StagedPath!));
    }

    [Fact]
    public async Task SecondTickWithSameVersionDoesNotReDownload()
    {
        using var ws = new TempWorkspace();
        var manifest = NewManifest("2.0.0");
        var zip = Encoding.UTF8.GetBytes("payload");
        var sha = Sha256Hex(zip);
        var (loop, _, handler) = BuildLoop(ws, manifest with { PackageSha256 = sha }, zip);

        await loop.TickAsync();
        var firstDownloads = handler.DownloadCount;
        await loop.TickAsync();

        Assert.Equal(firstDownloads, handler.DownloadCount);
        Assert.Equal(2, handler.CheckCount);
    }

    [Fact]
    public async Task AutoApplyDeferredUntilQuiescentReturnsTrue()
    {
        using var ws = new TempWorkspace();
        var manifest = NewManifest("2.0.0");
        var zip = Encoding.UTF8.GetBytes("payload");
        var sha = Sha256Hex(zip);
        var quiescent = false;
        var applyCalls = 0;
        var (loop, machine, _) = BuildLoop(ws, manifest with { PackageSha256 = sha }, zip,
            autoApply: true,
            quiescent: () => quiescent,
            apply: (_, _, _) => { applyCalls++; return Task.FromResult(true); });

        await loop.TickAsync();
        Assert.Equal(0, applyCalls);
        Assert.Equal(UpdaterStatus.StagedReady, machine.Snapshot.Status);

        quiescent = true;
        await loop.TickAsync();
        Assert.Equal(1, applyCalls);
    }

    [Fact]
    public async Task NoManifestLeavesMachineInNoUpdate()
    {
        using var ws = new TempWorkspace();
        var handler = new FakeHandler(null, Array.Empty<byte>());
        using var http = new HttpClient(handler);
        var client = new HttpUpdateClient(http);
        var machine = new UpdaterStateMachine();
        var opts = new UpdateClientLoopOptions(
            BaseUrl: "http://example/api/update",
            Channel: "stable", Target: "win-x64",
            CurrentVersion: "1.0.0",
            StagingDirectory: ws.Root,
            PollInterval: TimeSpan.FromMinutes(1));
        using var loop = new UpdateClientLoop(opts, client, machine);

        await loop.TickAsync();

        Assert.Equal(UpdaterStatus.NoUpdate, machine.Snapshot.Status);
        Assert.Null(loop.StagedVersion);
    }

    private static (UpdateClientLoop loop, UpdaterStateMachine machine, FakeHandler handler) BuildLoop(
        TempWorkspace ws, UpdateManifest manifest, byte[] zip,
        bool autoApply = false,
        Func<bool>? quiescent = null,
        Func<UpdateManifest, string, CancellationToken, Task<bool>>? apply = null)
    {
        var handler = new FakeHandler(manifest, zip);
        var http = new HttpClient(handler);
        var client = new HttpUpdateClient(http);
        var machine = new UpdaterStateMachine();
        var opts = new UpdateClientLoopOptions(
            BaseUrl: "http://example/api/update",
            Channel: "stable", Target: "win-x64",
            CurrentVersion: "1.0.0",
            StagingDirectory: ws.Root,
            PollInterval: TimeSpan.FromMinutes(1)) { AutoApply = autoApply };
        var loop = new UpdateClientLoop(opts, client, machine, quiescent, apply);
        return (loop, machine, handler);
    }

    private static UpdateManifest NewManifest(string version) => new(
        Version: version, Channel: "stable", Target: "win-x64",
        PackageUrl: "http://example/pkg.zip",
        PackageSha256: "",
        PackageSize: 7,
        Kind: UpdatePackageKind.Full,
        Notes: null,
        PublishedAt: DateTimeOffset.UtcNow);

    private static string Sha256Hex(byte[] bytes)
    {
        using var sha = System.Security.Cryptography.SHA256.Create();
        return Convert.ToHexString(sha.ComputeHash(bytes)).ToLowerInvariant();
    }

    private sealed class FakeHandler : HttpMessageHandler
    {
        private readonly UpdateManifest? _manifest;
        private readonly byte[] _zip;
        public int CheckCount;
        public int DownloadCount;
        public FakeHandler(UpdateManifest? manifest, byte[] zip) { _manifest = manifest; _zip = zip; }

        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken ct)
        {
            var url = request.RequestUri!.ToString();
            if (url.Contains("/latest"))
            {
                CheckCount++;
                if (_manifest is null)
                    return Task.FromResult(new HttpResponseMessage(HttpStatusCode.NotFound));
                var json = JsonSerializer.Serialize(_manifest);
                return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent(json, Encoding.UTF8, "application/json"),
                });
            }
            DownloadCount++;
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new ByteArrayContent(_zip),
            });
        }
    }

    private sealed class TempWorkspace : IDisposable
    {
        public string Root { get; } = Path.Combine(Path.GetTempPath(),
            "saoauto-loop-" + Guid.NewGuid().ToString("N"));
        public TempWorkspace() => Directory.CreateDirectory(Root);
        public void Dispose()
        {
            try { if (Directory.Exists(Root)) Directory.Delete(Root, true); }
            catch { /* swallow */ }
        }
    }
}
