using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Shared host services passed to bridge contributors.
/// </summary>
public sealed record BridgeContributorContext(
    BridgeRouter Router,
    BridgeEventBroadcaster Broadcaster,
    SettingsManager Settings,
    GameStateManager States);
