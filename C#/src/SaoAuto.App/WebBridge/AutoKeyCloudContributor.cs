using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Contributes Auto-Key cloud WebView commands through the generic bridge contributor seam.
/// </summary>
public sealed class AutoKeyCloudContributor : IBridgeContributor
{
    private readonly Func<SettingsManager, AutoKeyCloudClient> _clientFactory;

    public AutoKeyCloudContributor(Func<SettingsManager, AutoKeyCloudClient>? clientFactory = null)
    {
        _clientFactory = clientFactory ?? (settings => AutoKeyCloudClient.FromSettings(settings));
    }

    public string Name => "autokey-cloud";

    public IDisposable Attach(BridgeContributorContext context)
    {
        if (context is null) throw new ArgumentNullException(nameof(context));

        var client = _clientFactory(context.Settings)
            ?? throw new InvalidOperationException("Auto-Key cloud client factory returned null.");
        try
        {
            var bridge = new AutoKeyCloudBridge(
                context.Router,
                client,
                context.Settings,
                clientFromSettings: _clientFactory);
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
        private AutoKeyCloudBridge? _bridge;
        private AutoKeyCloudClient? _client;

        public Attachment(AutoKeyCloudBridge bridge, AutoKeyCloudClient client)
        {
            _bridge = bridge;
            _client = client;
        }

        public void Dispose()
        {
            _bridge?.Dispose();
            _bridge = null;
            _client?.Dispose();
            _client = null;
        }
    }
}
