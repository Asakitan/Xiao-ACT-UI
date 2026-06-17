using SaoAuto.Core.Automation;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Contributes DPS WebView commands through the generic bridge contributor seam.
/// </summary>
public sealed class DpsContributor : IBridgeContributor
{
    private readonly Action? _reset;
    private readonly Func<DpsSnapshot?>? _lastReport;
    private readonly DpsTracker? _tracker;

    public DpsContributor(
        Action? reset = null,
        Func<DpsSnapshot?>? lastReport = null,
        DpsTracker? tracker = null)
    {
        _reset = reset;
        _lastReport = lastReport;
        _tracker = tracker;
    }

    public string Name => "dps";

    public IDisposable Attach(BridgeContributorContext context)
    {
        if (context is null) throw new ArgumentNullException(nameof(context));
        return new DpsBridge(context.Router, _reset, _lastReport, context.Settings, _tracker);
    }
}
