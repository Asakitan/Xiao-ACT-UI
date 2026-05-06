using System.Diagnostics;
using System.Text.Json.Nodes;
using SaoAuto.Core.Updater;

// SaoAuto.UpdateApply.exe
//
// Mirrors the Python `update_apply.main()` entry point. Usage:
//   SaoAuto.UpdateApply.exe <pending-meta.json>
//
// The pending-meta JSON tells us the staged package, install base, host PID
// to wait for, and optional restart target. We wait for the host to exit,
// run ApplyEngine, then optionally restart the host. All stdout/stderr is
// also written to <install-base>/logs/update_apply-<utc>.log.

if (args.Length == 0)
{
    Console.Error.WriteLine("usage: SaoAuto.UpdateApply.exe <pending-meta.json>");
    return 64;
}

var metaPath = args[0];
if (!File.Exists(metaPath))
{
    Console.Error.WriteLine($"meta not found: {metaPath}");
    return 65;
}

var json = JsonNode.Parse(File.ReadAllText(metaPath))?.AsObject()
           ?? throw new InvalidOperationException("invalid pending-meta JSON");
var meta = PendingApplyMeta.FromJson(json);

EnsureLogDir(meta.InstallBase);
var logPath = Path.Combine(meta.InstallBase, "logs",
    $"update_apply-{DateTime.UtcNow:yyyyMMdd-HHmmss}.log");
using var logWriter = new StreamWriter(logPath, append: true) { AutoFlush = true };
void Log(string msg) { Console.WriteLine(msg); logWriter.WriteLine($"[{DateTime.UtcNow:O}] {msg}"); }

Log($"[apply] meta={metaPath} pkg={meta.PackagePath} base={meta.InstallBase} hostPid={meta.HostPid}");

if (meta.HostPid > 0) WaitForExit(meta.HostPid, TimeSpan.FromSeconds(30), Log);

var engine = new ApplyEngine();
var outcome = engine.ApplyZip(new ApplyEngine.ApplyOptions(
    PackagePath: meta.PackagePath,
    InstallBase: meta.InstallBase,
    PendingMetaPath: metaPath,
    AllowTopLevelExe: false));

Log($"[apply] success={outcome.Success} replaced={outcome.FilesReplaced} rolled={outcome.FilesRolledBack}");
foreach (var e in outcome.Errors) Log($"[apply] err: {e}");

if (outcome.Success)
{
    SafeDelete(meta.PackagePath);
    SafeDelete(metaPath);
    if (!string.IsNullOrWhiteSpace(meta.RestartTarget) && File.Exists(meta.RestartTarget))
    {
        try
        {
            Process.Start(new ProcessStartInfo(meta.RestartTarget!) { UseShellExecute = true, WorkingDirectory = meta.InstallBase });
            Log($"[apply] launched {meta.RestartTarget}");
        }
        catch (Exception ex) { Log($"[apply] restart failed: {ex.Message}"); }
    }
    return 0;
}
return 1;

static void EnsureLogDir(string baseDir)
{
    try { Directory.CreateDirectory(Path.Combine(baseDir, "logs")); } catch { }
}

static void WaitForExit(int pid, TimeSpan timeout, Action<string> log)
{
    try
    {
        using var p = Process.GetProcessById(pid);
        if (!p.WaitForExit((int)timeout.TotalMilliseconds))
            log($"[apply] WARN: host pid {pid} did not exit after {timeout.TotalSeconds}s; proceeding anyway");
    }
    catch (ArgumentException) { /* already gone */ }
    catch (Exception ex) { log($"[apply] WARN: WaitForExit({pid}) threw {ex.Message}"); }
}

static void SafeDelete(string p) { try { if (File.Exists(p)) File.Delete(p); } catch { } }
