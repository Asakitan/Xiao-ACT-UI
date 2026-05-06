using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.Updater;

/// <summary>
/// Deterministic apply engine ported from <c>update_apply.py</c>.
/// Owns the zip-extract → per-file rename → replace → rollback flow.
/// The standalone <c>SaoAuto.UpdateApply.exe</c> is a thin shell over
/// this class so the same logic is unit-testable in-process.
///
/// Out of scope (deferred): tk UI window, runtime ABI cleanup details,
/// PID-watch via <c>WaitForSingleObject</c> (host responsibility).
/// </summary>
public sealed class ApplyEngine
{
    private readonly ILogger _log;
    private readonly Func<string, string, string, bool> _replaceWithRetry;

    public sealed record ApplyOptions(
        string PackagePath,
        string InstallBase,
        string? PendingMetaPath = null,
        bool AllowTopLevelExe = false,
        int RetriesPerFile = 6,
        int DelayMs = 200);

    public sealed record ApplyOutcome(
        bool Success,
        int FilesReplaced,
        int FilesRolledBack,
        IReadOnlyList<string> Errors)
    {
        public static ApplyOutcome Ok(int n) => new(true, n, 0, Array.Empty<string>());
        public static ApplyOutcome Fail(int n, int rollback, IReadOnlyList<string> errs) =>
            new(false, n, rollback, errs);
    }

    public ApplyEngine(
        ILogger? logger = null,
        Func<string, string, string, bool>? replaceWithRetry = null)
    {
        _log = logger ?? NullLogger.Instance;
        _replaceWithRetry = replaceWithRetry ?? DefaultReplaceWithRetry;
    }

    /// <summary>
    /// Apply a zip package atop <c>InstallBase</c>. Each file is staged via
    /// <c>{dst}.new</c>, then promoted via <see cref="File.Replace(string, string, string)"/>
    /// (which atomically swaps and keeps a <c>.bak</c>). On any failure
    /// inside the loop we restore from <c>.bak</c> backups and report what
    /// still needs the user's attention.
    /// </summary>
    public ApplyOutcome ApplyZip(ApplyOptions opts)
    {
        if (!File.Exists(opts.PackagePath)) return ApplyOutcome.Fail(0, 0, new[] { $"package not found: {opts.PackagePath}" });
        Directory.CreateDirectory(opts.InstallBase);

        var promoted = new List<(string dst, string bak)>();
        var errors = new List<string>();
        int rolled = 0;

        try
        {
            using var zip = ZipFile.OpenRead(opts.PackagePath);
            foreach (var entry in zip.Entries)
            {
                if (entry.FullName.EndsWith("/")) continue;
                if (!opts.AllowTopLevelExe && IsTopLevelExe(entry.FullName))
                {
                    _log.LogDebug("[Apply] skip top-level exe {Name}", entry.FullName);
                    continue;
                }

                var rel = NormalizeRel(entry.FullName);
                var dst = Path.Combine(opts.InstallBase, rel);
                var newPath = dst + ".new";
                var bakPath = dst + ".bak";

                Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
                using (var src = entry.Open())
                using (var fs = File.Create(newPath))
                {
                    src.CopyTo(fs);
                }

                if (!_replaceWithRetry(newPath, dst, bakPath))
                {
                    errors.Add($"replace failed: {rel}");
                    SafeDelete(newPath);
                    throw new IOException($"replace failed for {rel}");
                }
                promoted.Add((dst, bakPath));
            }
            // Success — drop the .bak shadows.
            foreach (var (_, bak) in promoted) SafeDelete(bak);
            return ApplyOutcome.Ok(promoted.Count);
        }
        catch (Exception ex)
        {
            errors.Add(ex.Message);
            // Roll back in reverse order.
            for (int i = promoted.Count - 1; i >= 0; i--)
            {
                var (dst, bak) = promoted[i];
                if (!File.Exists(bak)) continue;
                try { File.Move(bak, dst, overwrite: true); rolled++; }
                catch (Exception rex) { errors.Add($"rollback failed for {dst}: {rex.Message}"); }
            }
            return ApplyOutcome.Fail(promoted.Count, rolled, errors);
        }
    }

    /// <summary>
    /// SHA-256 of a file as lowercase hex. Used by DevPublish to compute
    /// <see cref="UpdateManifest.PackageSha256"/> and by the host to verify
    /// downloads before handoff.
    /// </summary>
    public static string Sha256Hex(string path)
    {
        using var sha = SHA256.Create();
        using var fs = File.OpenRead(path);
        var hash = sha.ComputeHash(fs);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }

    private static bool IsTopLevelExe(string entryName)
    {
        if (entryName.Contains('/') || entryName.Contains('\\')) return false;
        return entryName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase);
    }

    private static string NormalizeRel(string rel) =>
        rel.Replace('/', Path.DirectorySeparatorChar)
           .Replace('\\', Path.DirectorySeparatorChar)
           .TrimStart(Path.DirectorySeparatorChar);

    private static bool DefaultReplaceWithRetry(string src, string dst, string bak)
    {
        for (int attempt = 0; attempt < 6; attempt++)
        {
            try
            {
                if (File.Exists(dst))
                {
                    File.Replace(src, dst, bak, ignoreMetadataErrors: true);
                }
                else
                {
                    File.Move(src, dst);
                }
                return true;
            }
            catch (IOException) { Thread.Sleep(200 * (attempt + 1)); }
            catch (UnauthorizedAccessException) { Thread.Sleep(200 * (attempt + 1)); }
        }
        return false;
    }

    private static void SafeDelete(string p) { try { if (File.Exists(p)) File.Delete(p); } catch { } }
}

/// <summary>
/// Pending-meta blob written by the host before launching UpdateApply.exe.
/// Mirrors the JSON shape of <c>update_apply._load_pending_meta</c>.
/// </summary>
public sealed record PendingApplyMeta(
    string PackagePath,
    string InstallBase,
    int HostPid,
    string? RestartTarget,
    string? Channel,
    string? Version)
{
    public JsonObject ToJson() => new()
    {
        ["package_path"] = PackagePath,
        ["install_base"] = InstallBase,
        ["host_pid"] = HostPid,
        ["restart_target"] = RestartTarget,
        ["channel"] = Channel,
        ["version"] = Version,
    };

    public static PendingApplyMeta FromJson(JsonObject obj) => new(
        PackagePath: obj["package_path"]?.GetValue<string>() ?? "",
        InstallBase: obj["install_base"]?.GetValue<string>() ?? "",
        HostPid: obj["host_pid"]?.GetValue<int>() ?? 0,
        RestartTarget: obj["restart_target"]?.GetValue<string>(),
        Channel: obj["channel"]?.GetValue<string>(),
        Version: obj["version"]?.GetValue<string>());
}
