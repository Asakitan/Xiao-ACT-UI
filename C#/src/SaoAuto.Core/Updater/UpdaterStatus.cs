namespace SaoAuto.Core.Updater;

/// <summary>
/// Lifecycle of the updater state machine. Mirrors Python's
/// <c>sao_updater.UpdateStatus</c> enum names (Idle / Checking /
/// AvailableReady / Downloading / DownloadedReady / Applying / ApplyFailed).
/// </summary>
public enum UpdaterStatus
{
    Idle,
    Checking,
    NoUpdate,
    UpdateAvailable,
    Downloading,
    StagedReady,
    Applying,
    ApplyFailed,
}

public sealed record UpdaterState(
    UpdaterStatus Status,
    UpdateManifest? Latest,
    double DownloadProgress,
    string? Error)
{
    public static readonly UpdaterState Idle = new(UpdaterStatus.Idle, null, 0, null);
}
