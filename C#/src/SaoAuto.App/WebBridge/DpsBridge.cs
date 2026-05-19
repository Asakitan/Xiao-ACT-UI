using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S167 — Registers <c>dps.reset_combat</c> and
/// <c>dps.show_last_report</c> on a <see cref="BridgeRouter"/>,
/// backed by thin delegates so the runner site can wire whatever
/// <see cref="DpsTracker"/> is live without coupling the bridge to
/// the packet-runtime lifetime. When the delegates are <c>null</c>
/// (e.g. packet runtime never started), commands return a structured
/// <c>{error}</c> instead of throwing or no-oping silently.
///
/// Reply shapes:
/// <list type="bullet">
/// <item><c>dps.reset_combat</c> ← (no payload) →
///   <c>{reset:true}</c> or <c>{error:"dps_unavailable"}</c>.</item>
/// <item><c>dps.show_last_report</c> ← (no payload) →
///   <c>{report:string, duration_s:number, total_damage:number}</c>
///   or <c>{error:"no_report"|"dps_unavailable"}</c>.</item>
/// </list>
/// </summary>
public sealed class DpsBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly Action? _reset;
    private readonly Func<DpsSnapshot?>? _lastReport;
    private bool _disposed;

    public DpsBridge(BridgeRouter router, Action? reset, Func<DpsSnapshot?>? lastReport)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _reset = reset;
        _lastReport = lastReport;
        router.Register(BridgeCommands.ResetCombat, _ => HandleReset());
        router.Register(BridgeCommands.ShowLastDpsReport, _ => HandleLastReport());
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _router.Unregister(BridgeCommands.ResetCombat);
        _router.Unregister(BridgeCommands.ShowLastDpsReport);
    }

    private JsonObject HandleReset()
    {
        if (_reset is null) return new JsonObject { ["error"] = "dps_unavailable" };
        _reset();
        return new JsonObject { ["reset"] = true };
    }

    private JsonObject HandleLastReport()
    {
        if (_lastReport is null) return new JsonObject { ["error"] = "dps_unavailable" };
        var snap = _lastReport();
        if (snap is null) return new JsonObject { ["error"] = "no_report" };
        return new JsonObject
        {
            ["report"] = DpsTracker.FormatReport(snap),
            ["duration_s"] = snap.DurationSeconds,
            ["total_damage"] = snap.TotalDamage,
        };
    }
}
