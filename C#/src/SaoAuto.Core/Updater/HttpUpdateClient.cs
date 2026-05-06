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
    /// </summary>
    public async Task<bool> DownloadAsync(UpdateManifest manifest, string destinationPath,
        IProgress<double>? progress = null, CancellationToken cancellationToken = default)
    {
        try
        {
            using var response = await _http.GetAsync(manifest.PackageUrl,
                HttpCompletionOption.ResponseHeadersRead, cancellationToken).ConfigureAwait(false);
            response.EnsureSuccessStatusCode();
            var total = response.Content.Headers.ContentLength ?? manifest.PackageSize;
            await using var src = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);

            Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);
            await using var dst = File.Create(destinationPath);
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

            var hex = Convert.ToHexString(sha.Hash!).ToLowerInvariant();
            if (!string.IsNullOrEmpty(manifest.PackageSha256) &&
                !string.Equals(hex, manifest.PackageSha256, StringComparison.OrdinalIgnoreCase))
            {
                _log.LogError("[Updater] SHA256 mismatch (expected={Expected} actual={Actual})",
                    manifest.PackageSha256, hex);
                File.Delete(destinationPath);
                return false;
            }
            return true;
        }
        catch (Exception ex)
        {
            _log.LogError(ex, "[Updater] download failed: {Url}", manifest.PackageUrl);
            return false;
        }
    }
}
