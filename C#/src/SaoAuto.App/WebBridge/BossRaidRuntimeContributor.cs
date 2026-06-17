using SaoAuto.Core.Automation;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Contributes Boss Raid runtime WebView commands through the generic bridge contributor seam.
/// </summary>
public sealed class BossRaidRuntimeContributor : IBridgeContributor
{
    private readonly BossRaidEngine _engine;

    public BossRaidRuntimeContributor(BossRaidEngine engine)
    {
        _engine = engine ?? throw new ArgumentNullException(nameof(engine));
    }

    public string Name => "bossraid-runtime";

    public IDisposable Attach(BridgeContributorContext context)
    {
        if (context is null) throw new ArgumentNullException(nameof(context));
        return new BossRaidRuntimeBridge(context.Router, _engine, context.Settings, context.States);
    }
}
