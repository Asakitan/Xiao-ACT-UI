using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

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
    private readonly SettingsManager? _settings;
    private bool _disposed;

    public DpsBridge(
        BridgeRouter router,
        Action? reset,
        Func<DpsSnapshot?>? lastReport,
        SettingsManager? settings = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _reset = reset;
        _lastReport = lastReport;
        _settings = settings;
        router.Register(BridgeCommands.ResetCombat, _ => HandleReset());
        router.Register(BridgeCommands.ShowLastDpsReport, _ => HandleLastReport());
        // R8 / DPS-04: toggle command always registers; null settings just
        // surfaces {error:"settings_unavailable"} per the existing
        // dps_unavailable pattern.
        router.Register(BridgeCommands.DpsToggleEnabled, HandleToggle);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _router.Unregister(BridgeCommands.ResetCombat);
        _router.Unregister(BridgeCommands.ShowLastDpsReport);
        _router.Unregister(BridgeCommands.DpsToggleEnabled);
    }

    private JsonObject HandleToggle(JsonObject? payload)
    {
        if (_settings is null) return new JsonObject { ["error"] = "settings_unavailable" };
        // Payload `{enabled:bool}` writes; absent / wrong shape just reads.
        var current = _settings.Get<bool?>(SettingsKeys.DpsEnabled) ?? true;
        if (payload is not null
            && payload.TryGetPropertyValue("enabled", out var node)
            && node is not null)
        {
            try
            {
                var next = node.GetValue<bool>();
                if (next != current)
                {
                    _settings.Set(SettingsKeys.DpsEnabled, next);
                    _settings.Save();
                    current = next;
                }
            }
            catch (Exception)
            {
                return new JsonObject { ["error"] = "bad_payload" };
            }
        }
        return new JsonObject { ["enabled"] = current };
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
