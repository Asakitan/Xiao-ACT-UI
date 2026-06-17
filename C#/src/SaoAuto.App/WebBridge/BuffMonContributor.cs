namespace SaoAuto.App.WebBridge;

/// <summary>
/// Contributes Buff Monitor WebView commands through the generic bridge contributor seam.
/// </summary>
public sealed class BuffMonContributor : IBridgeContributor
{
    public string Name => "buffmon";

    public IDisposable Attach(BridgeContributorContext context)
    {
        if (context is null) throw new ArgumentNullException(nameof(context));
        return new BuffMonBridge(context.Settings, context.Router);
    }
}
