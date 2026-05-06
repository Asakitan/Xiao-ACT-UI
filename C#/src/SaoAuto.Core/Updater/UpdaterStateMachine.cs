using SaoAuto.Core.Configuration;

namespace SaoAuto.Core.Updater;

/// <summary>
/// Pure state-machine for the SaoAuto updater UI. The transport layer
/// (HTTP fetch, file download, signature verify, apply-helper handoff)
/// drives transitions by calling these methods; subscribers (WebView
/// bridge / entity status bar) listen on <see cref="StateChanged"/>.
/// </summary>
public sealed class UpdaterStateMachine
{
    private readonly object _gate = new();
    private UpdaterState _state = UpdaterState.Idle;

    public event Action<UpdaterState>? StateChanged;

    public UpdaterState Snapshot
    {
        get { lock (_gate) return _state; }
    }

    public void BeginCheck()
    {
        Set(_state with { Status = UpdaterStatus.Checking, Error = null });
    }

    public void CheckCompleted(UpdateManifest? latest, string currentVersion)
    {
        if (latest is null)
        {
            Set(new UpdaterState(UpdaterStatus.NoUpdate, null, 0, null));
            return;
        }
        var diff = AppVersion.Compare(latest.Version, currentVersion);
        if (diff > 0)
        {
            Set(new UpdaterState(UpdaterStatus.UpdateAvailable, latest, 0, null));
        }
        else
        {
            Set(new UpdaterState(UpdaterStatus.NoUpdate, latest, 0, null));
        }
    }

    public void BeginDownload()
    {
        if (_state.Latest is null) throw new InvalidOperationException("no manifest staged");
        Set(_state with { Status = UpdaterStatus.Downloading, DownloadProgress = 0, Error = null });
    }

    public void ReportProgress(double progress)
    {
        Set(_state with { DownloadProgress = Math.Clamp(progress, 0, 1) });
    }

    public void DownloadCompleted()
    {
        Set(_state with { Status = UpdaterStatus.StagedReady, DownloadProgress = 1, Error = null });
    }

    public void DownloadFailed(string error)
    {
        Set(_state with { Status = UpdaterStatus.UpdateAvailable, DownloadProgress = 0, Error = error });
    }

    public void BeginApply()
    {
        if (_state.Status != UpdaterStatus.StagedReady)
        {
            throw new InvalidOperationException("apply requires StagedReady");
        }
        Set(_state with { Status = UpdaterStatus.Applying, Error = null });
    }

    /// <summary>
    /// Apply handoff failed — restore the READY state so the UI can offer
    /// "retry apply". Mirrors Python's `_on_apply_update_failed` rule that
    /// the staged package is still present.
    /// </summary>
    public void ApplyFailed(string error)
    {
        Set(_state with { Status = UpdaterStatus.StagedReady, Error = error });
    }

    public void Reset() => Set(UpdaterState.Idle);

    private void Set(UpdaterState next)
    {
        UpdaterState toRaise;
        lock (_gate)
        {
            if (_state == next) return;
            _state = next;
            toRaise = next;
        }
        try { StateChanged?.Invoke(toRaise); }
        catch { /* swallow */ }
    }
}
