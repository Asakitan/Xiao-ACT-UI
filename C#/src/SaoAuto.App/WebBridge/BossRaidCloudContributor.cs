using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Contributes Boss Raid cloud WebView commands through the generic bridge contributor seam.
/// </summary>
public sealed class BossRaidCloudContributor : IBridgeContributor
{
    private readonly Func<SettingsManager, BossRaidCloudClient> _clientFactory;

    public BossRaidCloudContributor(Func<SettingsManager, BossRaidCloudClient>? clientFactory = null)
    {
        _clientFactory = clientFactory ?? (settings => BossRaidCloudClient.FromSettings(settings));
    }

    public string Name => "bossraid-cloud";

    public IDisposable Attach(BridgeContributorContext context)
    {
        if (context is null) throw new ArgumentNullException(nameof(context));
        var client = _clientFactory(context.Settings)
            ?? throw new InvalidOperationException("Boss Raid cloud client factory returned null.");
        try
        {
            var bridge = new BossRaidCloudBridge(
                context.Router,
                client,
                context.Settings,
                clientFromSettings: _clientFactory,
                states: context.States);
            return new Attachment(bridge, client);
        }
        catch
        {
            client.Dispose();
            throw;
        }
    }

    private sealed class Attachment : IDisposable
    {
        private BossRaidCloudBridge? _bridge;
        private BossRaidCloudClient? _client;

        public Attachment(BossRaidCloudBridge bridge, BossRaidCloudClient client)
        {
            _bridge = bridge;
            _client = client;
        }

        public void Dispose()
        {
            _bridge?.Dispose();
            _client?.Dispose();
            _bridge = null;
            _client = null;
        }
    }
}
