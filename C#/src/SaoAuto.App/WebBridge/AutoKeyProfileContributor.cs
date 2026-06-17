using SaoAuto.Core.Automation;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Contributes AutoKey profile WebView commands through the generic bridge contributor seam.
/// </summary>
public sealed class AutoKeyProfileContributor : IBridgeContributor
{
    public string Name => "autokey-profile";

    public IDisposable Attach(BridgeContributorContext context)
    {
        if (context is null) throw new ArgumentNullException(nameof(context));
        return new AutoKeyProfileBridge(new AutoKeyProfileService(context.Settings, context.States), context.Router);
    }
}
