namespace SaoAuto.App.WebBridge;

/// <summary>
/// Registers a bounded set of WebView bridge commands against a host context.
/// </summary>
public interface IBridgeContributor
{
    string Name { get; }

    IDisposable Attach(BridgeContributorContext context);
}
