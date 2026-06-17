namespace SaoAuto.App.WebBridge;

/// <summary>
/// Contributes the native ACT empty-state runtime through the generic WebView bridge seam.
/// </summary>
public sealed class ActRuntimeContributor : IBridgeContributor
{
    public string Name => "act-runtime";

    public IDisposable Attach(BridgeContributorContext context)
    {
        if (context is null) throw new ArgumentNullException(nameof(context));

        var runtime = new ActRuntime(context.Settings, context.States);
        return new ActBridge(context.Router, runtime.Handle);
    }
}
