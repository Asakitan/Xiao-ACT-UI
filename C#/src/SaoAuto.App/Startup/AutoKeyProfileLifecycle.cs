using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.App.Startup;

/// <summary>
/// S156 — App-layer DI seam for <see cref="AutoKeyProfileService"/>.
///
/// The service itself is stateless aside from its <c>SettingsManager</c>
/// reference, but the runners need a single shared instance so that a
/// future <see cref="WebBridge.AutoKeyProfileBridge"/> handler and a
/// future profile-editor UI both edit the same in-memory cfg surface.
///
/// Mirrors the lifecycle pattern used by AutoKey / HideSeek even though
/// the service has nothing to dispose — keeps the runner composition
/// shape uniform.
/// </summary>
public sealed class AutoKeyProfileLifecycle : IDisposable
{
    private bool _disposed;

    public AutoKeyProfileService Service { get; }

    public AutoKeyProfileLifecycle(SettingsManager settings, GameStateManager? states)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        Service = new AutoKeyProfileService(settings, states);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
    }
}
