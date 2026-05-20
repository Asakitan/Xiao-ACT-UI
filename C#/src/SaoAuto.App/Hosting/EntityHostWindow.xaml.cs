using System.Windows;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Logging;

namespace SaoAuto.App.Hosting;

public partial class EntityHostWindow : Window
{
    private static readonly ILogger _log = SaoLog.For("entity");

    public EntityHostWindow()
    {
        InitializeComponent();
    }

    /// <summary>S196 — apply the same Python-parity HUD geometry as
    /// <see cref="WebViewHostWindow"/>. Shared via
    /// <see cref="HudGeometry.Apply"/>.</summary>
    public void ApplyHudGeometry(SettingsManager? settings = null)
    {
        HudGeometry.Apply(this, settings, _log);
    }
}
