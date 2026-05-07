using System.Net.Http.Json;
using System.Security.Cryptography;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.Updater;

/// <summary>
/// HTTP-backed update client. Talks to the manifest endpoint exposed
/// by <c>SaoAuto.UpdateHost</c> (or any compatible Python
/// <c>update_host/app.py</c>) and downloads the package into the
/// staging directory. Verifies SHA256 before reporting success.
/// </summary>
public sealed class HttpUpdateClient
{
    private readonly HttpClient _http;
    private readonly ILogger _log;

    public HttpUpdateClient(HttpClient? http = null, ILogger<HttpUpdateClient>? logger = null)
    {
        _http = http ?? new HttpClient { Timeout = TimeSpan.FromSeconds(30) };
        _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    public async Task<UpdateManifest?> CheckLatestAsync(string baseUrl, string channel, string target,
        CancellationToken cancellationToken = default)
    {
        try
        {
            var url = $"{baseUrl.TrimEnd('/')}/latest?channel={channel}&target={target}";
            return await _http.GetFromJsonAsync<UpdateManifest>(url, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[Updater] check failed: {Url}", baseUrl);
            return null;
        }
    }

    /// <summary>
    /// Download the manifest's package URL into <paramref name="destinationPath"/>
    /// and verify the declared SHA256. Reports progress through
    /// <paramref name="progress"/> as a 0..1 value. Returns true on success.
    ///
    /// S104: writes to <c>{destinationPath}.part</c> first and atomic-renames
    /// on success (mirrors Python <c>sao_updater._http_download</c>'s
    /// <c>.part</c> + <c>os.replace</c> pattern). If the SHA mismatches, or
    /// the transfer throws, the partial file is removed so a future tick
    /// can re-download cleanly without an existing <c>destinationPath</c>
    /// being mistaken for a complete download.
    /// </summary>
    public async Task<bool> DownloadAsync(UpdateManifest manifest, string destinationPath,
        IProgress<double>? progress = null, CancellationToken cancellationToken = default)
    {
        var partPath = destinationPath + ".part";
        try
        {
            using var response = await _http.GetAsync(manifest.PackageUrl,
                HttpCompletionOption.ResponseHeadersRead, cancellationToken).ConfigureAwait(false);
            response.EnsureSuccessStatusCode();
            var total = response.Content.Headers.ContentLength ?? manifest.PackageSize;
            await using var src = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);

            Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);
            // Stale .part from a prior aborted download — start fresh.
            TryDelete(partPath);
            string hex;
            await using (var dst = File.Create(partPath))
            {
                using var sha = SHA256.Create();
                var buffer = new byte[81920];
                long copied = 0;
                int read;
                while ((read = await src.ReadAsync(buffer.AsMemory(), cancellationToken).ConfigureAwait(false)) > 0)
                {
                    await dst.WriteAsync(buffer.AsMemory(0, read), cancellationToken).ConfigureAwait(false);
                    sha.TransformBlock(buffer, 0, read, null, 0);
                    copied += read;
                    if (total > 0) progress?.Report(Math.Clamp((double)copied / total, 0, 1));
                }
                sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
                hex = Convert.ToHexString(sha.Hash!).ToLowerInvariant();
            }

            if (!string.IsNullOrEmpty(manifest.PackageSha256) &&
                !string.Equals(hex, manifest.PackageSha256, StringComparison.OrdinalIgnoreCase))
            {
                _log.LogError("[Updater] SHA256 mismatch (expected={Expected} actual={Actual})",
                    manifest.PackageSha256, hex);
                TryDelete(partPath);
                return false;
            }
            // Atomic-replace destination so a half-written file can never
            // masquerade as a complete download.
            if (File.Exists(destinationPath)) File.Delete(destinationPath);
            File.Move(partPath, destinationPath);
            return true;
        }
        catch (Exception ex)
        {
            _log.LogError(ex, "[Updater] download failed: {Url}", manifest.PackageUrl);
            TryDelete(partPath);
            return false;
        }
    }

    private static void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); }
        catch { /* best-effort cleanup */ }
    }
}
