using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using SaoAuto.Core.Updater;

namespace SaoAuto.Tests.Updater;

/// <summary>
/// S104 — Pin <see cref="HttpUpdateClient.DownloadAsync"/>'s
/// `.part` tempfile + atomic-replace pattern (port of Python
/// <c>sao_updater._http_download</c>'s <c>tmp_path = dst_path + ".part"</c>
/// + <c>os.replace</c> guard). On hash mismatch or transport error
/// the partial file must be removed so a future tick can re-download
/// without an existing destination being mistaken for a complete
/// download.
/// </summary>
public class Session104DownloadAtomicReplaceTests
{
    private sealed class TempDir : IDisposable
    {
        public string Path { get; } = System.IO.Path.Combine(
            System.IO.Path.GetTempPath(),
            "saoauto-s104-" + Guid.NewGuid().ToString("N"));
        public TempDir() => Directory.CreateDirectory(Path);
        public void Dispose()
        {
            try { if (Directory.Exists(Path)) Directory.Delete(Path, true); }
            catch { /* best-effort */ }
        }
    }

    private sealed class StubHandler : HttpMessageHandler
    {
        private readonly Func<HttpResponseMessage> _factory;
        public StubHandler(Func<HttpResponseMessage> factory) { _factory = factory; }
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken ct)
            => Task.FromResult(_factory());
    }

    private static string Sha256Hex(byte[] bytes)
    {
        using var sha = SHA256.Create();
        return Convert.ToHexString(sha.ComputeHash(bytes)).ToLowerInvariant();
    }

    private static UpdateManifest NewManifest(byte[] payload, string? expectedSha = null) =>
        new(
            Version: "9.9.9",
            Channel: "stable",
            Target: "win-x64",
            PackageUrl: "https://example.test/pkg.zip",
            PackageSha256: expectedSha ?? Sha256Hex(payload),
            PackageSize: payload.Length,
            Kind: UpdatePackageKind.Full,
            Notes: null,
            PublishedAt: DateTimeOffset.UtcNow);

    [Fact]
    public async Task SuccessfulDownload_AtomicallyMovesPartToDestination()
    {
        using var tmp = new TempDir();
        var dst = Path.Combine(tmp.Path, "pkg.zip");
        var payload = Encoding.UTF8.GetBytes("hello-payload");
        var client = new HttpUpdateClient(new HttpClient(new StubHandler(() =>
            new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(payload) })));

        var ok = await client.DownloadAsync(NewManifest(payload), dst);

        Assert.True(ok);
        Assert.True(File.Exists(dst));
        Assert.False(File.Exists(dst + ".part"),
            ".part tempfile must be moved away on success");
        Assert.Equal(payload, await File.ReadAllBytesAsync(dst));
    }

    [Fact]
    public async Task ShaMismatch_DeletesPartAndDoesNotCreateDestination()
    {
        using var tmp = new TempDir();
        var dst = Path.Combine(tmp.Path, "pkg.zip");
        var payload = Encoding.UTF8.GetBytes("hello-payload");
        var client = new HttpUpdateClient(new HttpClient(new StubHandler(() =>
            new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(payload) })));
        var manifest = NewManifest(payload, expectedSha: new string('0', 64));

        var ok = await client.DownloadAsync(manifest, dst);

        Assert.False(ok);
        Assert.False(File.Exists(dst),
            "destination must not exist when SHA256 fails");
        Assert.False(File.Exists(dst + ".part"),
            ".part must be cleaned up on hash mismatch");
    }

    [Fact]
    public async Task ShaMismatch_PreservesExistingDestinationFromPriorRun()
    {
        // Existing dst (e.g. from a previous successful download) must NOT
        // be deleted when a new attempt fails the hash check — the
        // atomic-replace guard means we only touch it on success.
        using var tmp = new TempDir();
        var dst = Path.Combine(tmp.Path, "pkg.zip");
        var prior = Encoding.UTF8.GetBytes("prior-known-good");
        await File.WriteAllBytesAsync(dst, prior);

        var bad = Encoding.UTF8.GetBytes("corrupted-payload");
        var client = new HttpUpdateClient(new HttpClient(new StubHandler(() =>
            new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(bad) })));
        var manifest = NewManifest(bad, expectedSha: new string('0', 64));

        var ok = await client.DownloadAsync(manifest, dst);

        Assert.False(ok);
        Assert.True(File.Exists(dst),
            "prior dst must survive a failed download");
        Assert.Equal(prior, await File.ReadAllBytesAsync(dst));
        Assert.False(File.Exists(dst + ".part"));
    }

    [Fact]
    public async Task TransportError_DeletesPartFile()
    {
        using var tmp = new TempDir();
        var dst = Path.Combine(tmp.Path, "pkg.zip");
        var client = new HttpUpdateClient(new HttpClient(new StubHandler(() =>
            new HttpResponseMessage(HttpStatusCode.InternalServerError))));
        var manifest = new UpdateManifest(
            Version: "9.9.9",
            Channel: "stable",
            Target: "win-x64",
            PackageUrl: "https://example.test/pkg.zip",
            PackageSha256: "deadbeef",
            PackageSize: 0,
            Kind: UpdatePackageKind.Full,
            Notes: null,
            PublishedAt: DateTimeOffset.UtcNow);

        var ok = await client.DownloadAsync(manifest, dst);

        Assert.False(ok);
        Assert.False(File.Exists(dst));
        Assert.False(File.Exists(dst + ".part"));
    }

    [Fact]
    public async Task StalePartFromPriorAbortedDownload_IsOverwritten()
    {
        // A prior crash could leave a .part behind; a fresh download
        // must not be confused by it (Python's open(tmp_path,"wb")
        // truncates; we delete-then-create for the same effect).
        using var tmp = new TempDir();
        var dst = Path.Combine(tmp.Path, "pkg.zip");
        await File.WriteAllBytesAsync(dst + ".part", new byte[] { 0xDE, 0xAD });

        var payload = Encoding.UTF8.GetBytes("fresh-payload");
        var client = new HttpUpdateClient(new HttpClient(new StubHandler(() =>
            new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(payload) })));

        var ok = await client.DownloadAsync(NewManifest(payload), dst);

        Assert.True(ok);
        Assert.Equal(payload, await File.ReadAllBytesAsync(dst));
        Assert.False(File.Exists(dst + ".part"));
    }

    [Fact]
    public async Task SuccessOverwritesExistingDestination()
    {
        using var tmp = new TempDir();
        var dst = Path.Combine(tmp.Path, "pkg.zip");
        await File.WriteAllBytesAsync(dst, Encoding.UTF8.GetBytes("old-version"));

        var payload = Encoding.UTF8.GetBytes("new-version");
        var client = new HttpUpdateClient(new HttpClient(new StubHandler(() =>
            new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(payload) })));

        var ok = await client.DownloadAsync(NewManifest(payload), dst);

        Assert.True(ok);
        Assert.Equal(payload, await File.ReadAllBytesAsync(dst));
    }
}
