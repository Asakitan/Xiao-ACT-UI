using System.Text.Json.Nodes;
using SaoAuto.Core.Updater;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S171 — Registers <c>updater.check</c>, <c>updater.download</c>, and
/// <c>updater.apply</c> on a <see cref="BridgeRouter"/>, backed by
/// async delegates the runner site can wire to
/// <see cref="UpdateClientLoop.TickAsync"/> /
/// <see cref="UpdateClientLoop.DownloadAsync"/> /
/// <see cref="UpdateClientLoop.ApplyAsync"/>.
///
/// Each handler kicks the supplied delegate off as fire-and-forget
/// (<see cref="Task.Run(Func{Task})"/>) and replies immediately with a
/// snapshot of the current <see cref="UpdaterState"/>. Live progress
/// reaches JS via the <see cref="BridgeEvents.UpdaterStatus"/> event
/// emitted from <see cref="UpdaterStateMachine.StateChanged"/> when a
/// broadcaster is supplied.
///
/// Reply shapes:
/// <list type="bullet">
/// <item>All three commands → <c>{status, version, progress, error,
///   queued}</c>. <c>queued:true</c> means the delegate was kicked
///   off; <c>queued:false</c> + <c>error:"unsupported"</c> means no
///   delegate is wired.</item>
/// </list>
/// </summary>
public sealed class UpdaterBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly UpdaterStateMachine _machine;
    private readonly BridgeEventBroadcaster? _broadcaster;
    private readonly Func<CancellationToken, Task>? _check;
    private readonly Func<CancellationToken, Task>? _download;
    private readonly Func<CancellationToken, Task>? _apply;
    private readonly Func<CancellationToken, Task<bool>>? _skip;
    private readonly Action<UpdaterState>? _stateChangedHandler;
    private bool _disposed;

    public UpdaterBridge(
        BridgeRouter router,
        UpdaterStateMachine machine,
        BridgeEventBroadcaster? broadcaster = null,
        Func<CancellationToken, Task>? check = null,
        Func<CancellationToken, Task>? download = null,
        Func<CancellationToken, Task>? apply = null,
        Func<CancellationToken, Task<bool>>? skip = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _machine = machine ?? throw new ArgumentNullException(nameof(machine));
        _broadcaster = broadcaster;
        _check = check;
        _download = download;
        _apply = apply;
        _skip = skip;

        router.Register(BridgeCommands.CheckUpdate, _ => HandleKick(_check));
        router.Register(BridgeCommands.DownloadUpdate, _ => HandleKick(_download));
        router.Register(BridgeCommands.ApplyUpdate, _ => HandleKick(_apply));
        router.Register(BridgeCommands.SkipUpdate, _ => HandleSkip());

        if (_broadcaster is not null)
        {
            _stateChangedHandler = OnStateChanged;
            _machine.StateChanged += _stateChangedHandler;
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_stateChangedHandler is not null)
        {
            _machine.StateChanged -= _stateChangedHandler;
        }
        _router.Unregister(BridgeCommands.CheckUpdate);
        _router.Unregister(BridgeCommands.DownloadUpdate);
        _router.Unregister(BridgeCommands.ApplyUpdate);
        _router.Unregister(BridgeCommands.SkipUpdate);
    }

    private JsonObject HandleKick(Func<CancellationToken, Task>? action)
    {
        if (action is null)
        {
            var reply = SnapshotToJson(_machine.Snapshot);
            reply["queued"] = false;
            reply["error"] = "unsupported";
            return reply;
        }

        // Fire and forget — actual progress will surface through
        // StateChanged → updater.status events. Swallow background
        // exceptions so the dispatcher doesn't see them.
        _ = Task.Run(async () =>
        {
            try { await action(CancellationToken.None).ConfigureAwait(false); }
            catch { /* swallow; state machine carries the error */ }
        });

        var ok = SnapshotToJson(_machine.Snapshot);
        ok["queued"] = true;
        return ok;
    }

    private JsonObject HandleSkip()
    {
        if (_skip is null)
        {
            var reply = SnapshotToJson(_machine.Snapshot);
            reply["ok"] = false;
            reply["skipped"] = false;
            reply["error"] = "unsupported";
            return reply;
        }

        try
        {
            var skipped = _skip(CancellationToken.None).GetAwaiter().GetResult();
            var reply = SnapshotToJson(_machine.Snapshot);
            reply["ok"] = skipped;
            reply["skipped"] = skipped;
            return reply;
        }
        catch (Exception ex)
        {
            var reply = SnapshotToJson(_machine.Snapshot);
            reply["ok"] = false;
            reply["skipped"] = false;
            reply["error"] = "skip_failed";
            reply["message"] = ex.Message;
            return reply;
        }
    }

    private void OnStateChanged(UpdaterState state)
    {
        try { _broadcaster?.Emit(BridgeEvents.UpdaterStatus, SnapshotToJson(state)); }
        catch { /* swallow */ }
    }

    private static JsonObject SnapshotToJson(UpdaterState state) => new()
    {
        ["status"] = state.Status.ToString(),
        ["version"] = state.Latest?.Version,
        ["progress"] = state.DownloadProgress,
        ["error"] = state.Error,
    };
}
