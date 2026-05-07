using SaoAuto.Core.State;

namespace SaoAuto.Core.Vision;

/// <summary>
/// S81c — Long-running host for <see cref="RecognitionTickEngine"/>.
/// Bit-faithful port of <c>RecognitionEngine._run</c>
/// (recognition.py 901–923): drive the tick on a worker thread,
/// sleep <c>1/NextFps - elapsed</c> between ticks, project each
/// tick result onto a <see cref="GameStateManager"/>.
///
/// Pure adapter — no <c>Microsoft.Extensions.Hosting</c> dep, so the
/// Core assembly stays plug-and-play. The App layer can wrap this
/// in a <c>BackgroundService</c> with one trivial subclass.
///
/// Threading: <see cref="StartAsync"/> spins a single dedicated
/// task; <see cref="StopAsync"/> cancels it and awaits.
/// <see cref="RunOnceAsync"/> is exposed for unit tests so the
/// loop body can be exercised without a real timer.
/// </summary>
public sealed class RecognitionTickHost : IDisposable
{
    private readonly RecognitionTickEngine _engine;
    private readonly GameStateManager _states;
    private readonly Func<double> _clock;
    private readonly Func<int, CancellationToken, Task> _delay;
    private readonly double _minSleepSeconds;
    private readonly IReadOnlyList<int>? _watchedSlots;

    private CancellationTokenSource? _cts;
    private Task? _runner;
    private double _lastTickStartedAt;

    public RecognitionTickHost(
        RecognitionTickEngine engine,
        GameStateManager states,
        Func<double>? clock = null,
        Func<int, CancellationToken, Task>? delay = null,
        double minSleepSeconds = 0.01,
        IReadOnlyList<int>? watchedSlots = null)
    {
        _engine = engine ?? throw new ArgumentNullException(nameof(engine));
        _states = states ?? throw new ArgumentNullException(nameof(states));
        _clock = clock ?? DefaultClock;
        _delay = delay ?? DefaultDelay;
        _minSleepSeconds = minSleepSeconds;
        _watchedSlots = watchedSlots;
    }

    public bool IsRunning => _runner is { IsCompleted: false };

    public Task StartAsync(CancellationToken ct = default)
    {
        if (IsRunning) return Task.CompletedTask;
        _cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        var token = _cts.Token;
        _runner = Task.Run(() => RunLoopAsync(token), token);
        return Task.CompletedTask;
    }

    public async Task StopAsync()
    {
        if (_cts is null) return;
        _cts.Cancel();
        try { if (_runner is not null) await _runner.ConfigureAwait(false); }
        catch (OperationCanceledException) { }
        _cts.Dispose();
        _cts = null;
        _runner = null;
    }

    /// <summary>
    /// Test seam: drive one tick + project, return the
    /// computed sleep-millis the loop would have used. Doesn't
    /// actually sleep.
    /// </summary>
    public int RunOnceAsync()
    {
        var start = _clock();
        var result = SafeTick();
        Project(result);
        var elapsed = _clock() - start;
        var period = result.NextFps > 0 ? 1.0 / result.NextFps : 1.0;
        var sleep = Math.Max(_minSleepSeconds, period - elapsed);
        _lastTickStartedAt = start;
        return (int)Math.Round(sleep * 1000.0);
    }

    private async Task RunLoopAsync(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            var start = _clock();
            var result = SafeTick();
            Project(result);
            var elapsed = _clock() - start;
            var period = result.NextFps > 0 ? 1.0 / result.NextFps : 1.0;
            var sleep = Math.Max(_minSleepSeconds, period - elapsed);
            try { await _delay((int)Math.Round(sleep * 1000.0), token).ConfigureAwait(false); }
            catch (OperationCanceledException) { return; }
        }
    }

    private RecognitionTickResult SafeTick()
    {
        try { return _engine.Tick(); }
        catch (Exception ex)
        {
            return new RecognitionTickResult(
                RecognitionOk: false,
                ErrorMsg: ex.Message,
                Window: null,
                StaminaPct: null,
                StaminaOffline: false,
                StaminaJustWentOffline: false,
                StaminaJustRecovered: false,
                NextFps: 1.0,
                FrameSource: FrameSource.None,
                SkillSlots: Array.Empty<SkillSlotResult>());
        }
    }

    private void Project(RecognitionTickResult r)
    {
        // S103: route StaminaPct through ApplyPartial so the [0,1] clamp
        // runs (vision interpolation can produce values slightly outside
        // the range). The other recognition fields (RecognitionOk,
        // ErrorMsg, StaminaOffline, SkillSlots, BurstReady) ride along
        // via the extraMutate hook so the projection still emits one
        // atomic snapshot per tick — preserving the S101 narrow-channel
        // single-fire contract.
        var slots = RecognitionSkillSlotProjector.Project(r.SkillSlots);
        var burstReady = BurstReadyCalculator.Compute(slots, _watchedSlots);
        var partial = new StatePartial
        {
            StaminaPct = r.StaminaPct,
        };
        _states.ApplyPartial(partial, s => s with
        {
            RecognitionOk = r.RecognitionOk,
            ErrorMsg = r.ErrorMsg,
            StaminaOffline = r.StaminaOffline,
            SkillSlots = slots,
            BurstReady = burstReady,
        });
    }

    public void Dispose()
    {
        try { StopAsync().GetAwaiter().GetResult(); } catch { }
    }

    private static double DefaultClock() =>
        DateTime.UtcNow.Subtract(DateTime.UnixEpoch).TotalSeconds;

    private static Task DefaultDelay(int ms, CancellationToken ct) =>
        Task.Delay(ms, ct);
}
