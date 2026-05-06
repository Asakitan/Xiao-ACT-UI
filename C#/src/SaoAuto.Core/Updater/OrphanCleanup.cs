namespace SaoAuto.Core.Updater;

/// <summary>
/// Orphan-file cleanup ported from <c>update_apply.py</c>:
/// <c>_normalize_rel</c>, <c>_safe_remove_orphan</c>, and
/// <c>_apply_removed_files</c>. Driven by a manifest's
/// <c>removed_files</c> list — older versions of the package may
/// leave behind files that newer versions no longer ship; this
/// stage backs them up under <c>__removed__</c> in the backup root
/// then deletes the live copies, with safety rails to refuse paths
/// that escape the install base or target the launcher / updater
/// executables.
/// </summary>
public static class OrphanCleanup
{
    public static readonly IReadOnlySet<string> ProtectedExeNames =
        new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "update.exe",
            "xiaoactui.exe",
        };

    /// <summary>
    /// Normalize a manifest-supplied relative path. Uses forward slashes,
    /// strips outer whitespace, and rejects empty / absolute / parent-
    /// traversal paths. Throws <see cref="ArgumentException"/> on invalid input.
    /// </summary>
    public static string NormalizeRel(string rel)
    {
        if (rel is null) throw new ArgumentException("relative path is null");
        var trimmed = rel.Trim().Replace('\\', '/');
        if (trimmed.Length == 0) throw new ArgumentException("relative path is empty");
        if (Path.IsPathRooted(trimmed)) throw new ArgumentException("relative path must not be absolute");
        foreach (var seg in trimmed.Split('/'))
        {
            if (seg == "..") throw new ArgumentException("relative path must not contain parent traversal");
        }
        return trimmed;
    }

    /// <summary>
    /// Try to delete a single orphan file under <paramref name="installBase"/>.
    /// Refuses top-level executables, the launcher / updater binaries, paths
    /// that escape the base, and directory targets. Treats missing files as
    /// already-clean. Returns whether the file was removed (or was absent)
    /// and a short human-readable reason.
    /// </summary>
    public static (bool Removed, string Reason) SafeRemoveOrphan(string installBase, string rel)
    {
        string norm;
        try { norm = NormalizeRel(rel); }
        catch (Exception ex) { return (false, $"非法路径: {ex.Message}"); }

        var low = norm.ToLowerInvariant();
        if (low.EndsWith(".exe", StringComparison.Ordinal) && !norm.Contains('/'))
            return (false, "禁止删除顶层 exe");
        if (ProtectedExeNames.Contains(low))
            return (false, "禁止删除启动器/更新器");

        var absBase = Path.GetFullPath(installBase);
        var absPath = Path.GetFullPath(Path.Combine(absBase, norm));
        var sep = Path.DirectorySeparatorChar;
        if (string.Equals(absPath, absBase, StringComparison.OrdinalIgnoreCase) ||
            !absPath.StartsWith(absBase + sep, StringComparison.OrdinalIgnoreCase))
            return (false, "路径逃出 base");

        if (!File.Exists(absPath))
        {
            if (Directory.Exists(absPath)) return (false, "拒绝目录删除");
            return (true, "absent");
        }
        try
        {
            File.Delete(absPath);
            return (true, "removed");
        }
        catch (Exception ex)
        {
            return (false, ex.Message);
        }
    }

    /// <summary>
    /// Drive the <c>removed_files</c> stage. Backs each live file up under
    /// <c>{backupRoot}/__removed__/{rel}</c> before deletion so the apply
    /// engine can restore on rollback. De-duplicates entries and skips
    /// invalid / refused paths. Returns the relative paths actually removed
    /// + the list of <c>(rel, backupPath)</c> pairs.
    /// </summary>
    public static OrphanCleanupResult ApplyRemovedFiles(
        string installBase, IReadOnlyList<string> rels, string backupRoot)
    {
        var removed = new List<string>();
        var backups = new List<(string Rel, string BackupPath)>();
        if (rels is null || rels.Count == 0) return new(removed, backups);

        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var ordered = new List<string>(rels.Count);
        foreach (var rel in rels)
        {
            if (string.IsNullOrWhiteSpace(rel)) continue;
            var key = rel.Replace('\\', '/').Trim();
            if (!seen.Add(key)) continue;
            ordered.Add(rel);
        }

        foreach (var rel in ordered)
        {
            string norm;
            try { norm = NormalizeRel(rel); } catch { norm = rel; }
            var absPath = Path.GetFullPath(Path.Combine(installBase, norm));
            if (File.Exists(absPath))
            {
                try
                {
                    var backupPath = Path.Combine(backupRoot, "__removed__", norm);
                    var dir = Path.GetDirectoryName(backupPath);
                    if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                    File.Copy(absPath, backupPath, overwrite: true);
                    backups.Add((norm, backupPath));
                }
                catch { /* swallow — best-effort backup, mirrors Python */ }
            }
            var (ok, _) = SafeRemoveOrphan(installBase, rel);
            if (ok) removed.Add(rel);
        }
        return new OrphanCleanupResult(removed, backups);
    }

    /// <summary>
    /// Restore a previously-cleaned-up file set after a rollback. Safe to
    /// call on a partial backup list (skips entries whose backup path is
    /// missing).
    /// </summary>
    public static int RestoreRemovedBackups(
        string installBase, IReadOnlyList<(string Rel, string BackupPath)> backups)
    {
        var restored = 0;
        foreach (var (rel, backupPath) in backups)
        {
            if (!File.Exists(backupPath)) continue;
            try
            {
                var dst = Path.GetFullPath(Path.Combine(installBase, NormalizeRel(rel)));
                var dir = Path.GetDirectoryName(dst);
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                File.Copy(backupPath, dst, overwrite: true);
                restored++;
            }
            catch { /* swallow individual failures, continue */ }
        }
        return restored;
    }
}

public sealed record OrphanCleanupResult(
    IReadOnlyList<string> RemovedRels,
    IReadOnlyList<(string Rel, string BackupPath)> Backups);
