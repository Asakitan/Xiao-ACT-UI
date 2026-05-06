using Microsoft.Extensions.Logging;
using SaoAuto.Core.Logging;

namespace SaoAuto.App.Updater;

/// <summary>
/// Mirrors Python <c>main._register_apply_on_exit</c>: at process exit, if a
/// staged update package is pending, hand off to the helper. Real updater wiring
/// lands in Session 12; this placeholder only logs so we can verify the hook
/// fires on graceful shutdown without accidentally launching anything.
/// </summary>
public static class ApplyOnExitHook
{
    private static int _registered;

    public static void Register()
    {
        if (Interlocked.Exchange(ref _registered, 1) == 1) return;
        AppDomain.CurrentDomain.ProcessExit += OnProcessExit;
    }

    private static void OnProcessExit(object? sender, EventArgs e)
    {
        var log = SaoLog.For("updater");
        log.LogInformation("ProcessExit: apply-on-exit hook fired (Session 12 will replace this with real updater handoff)");
    }
}
