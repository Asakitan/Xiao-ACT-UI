using System.Collections.Immutable;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Core.Automation;

/// <summary>
/// S137 — background tick host for the spec-runtime AutoKey path.
/// Mirrors Python's <c>AutoKeyEngine._run</c> + <c>_tick</c> at
/// auto_key_engine.py 638–697: per tick, reload the active profile,
/// build a context off the live <see cref="GameState"/>, run gate
/// checks (enabled / foreground / death / recognition-or-packet),
/// then delegate to <see cref="AutoKeySpecRuntime.Tick"/>.
///
/// Pure Core type — no Win32 dependency. The foreground gate is
/// injected as a <see cref="Func{Boolean}"/> so App layer can wire
/// `WindowLocator`-based foreground detection without dragging
/// user32 into Core.
/// </summary>
public sealed class AutoKeyTickHost : IAsyncDisposable, IDisposable
{
    private readonly SettingsManager _settings;
    private readonly GameStateManager _states;
    private readonly AutoKeySpecRuntime _runtime;
    private readonly Func<bool> _foregroundProbe;
    private readonly Func<DateTimeOffset> _clock;
    private readonly ILogger _log;
    private readonly CancellationTokenSource _shutdown = new();
    private CancellationTokenSource? _runCts;
    private Task? _pump;
    private string _lastProfileId = "";
    private bool _disposed;

    public AutoKeyTickHost(
        SettingsManager settings,
        GameStateManager states,
        AutoKeySpecRuntime runtime,
        Func<bool>? foregroundProbe = null,
        Func<DateTimeOffset>? clock = null,
        ILogger<AutoKeyTickHost>? logger = null)
    {
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _states = states ?? throw new ArgumentNullException(nameof(states));
        _runtime = runtime ?? throw new ArgumentNullException(nameof(runtime));
        _foregroundProbe = foregroundProbe ?? (() => true);
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _log = (ILogger?)logger ?? NullLogger.Instance;
    }

    public string LastReason { get; private set; } = "init";
    public bool LastActive { get; private set; }
    public string LastActiveProfileId { get; private set; } = "";
    public string? LastFiredActionId { get; private set; }
    public long TickCount { get; private set; }

    /// <summary>S139 — pass-through of <see cref="AutoKeySpecRuntime.LastBlockReasons"/>.</summary>
    public IReadOnlyDictionary<string, string> LastBlockReasons => _runtime.LastBlockReasons;

    /// <summary>S140 — pass-through of <see cref="AutoKeySpecRuntime.LastCooldownRemainingMs"/>.</summary>
    public IReadOnlyDictionary<string, int> LastCooldownRemainingMs => _runtime.LastCooldownRemainingMs;

    /// <summary>S142 — pass-through of <see cref="AutoKeySpecRuntime.LastReadyForMs"/>.</summary>
    public IReadOnlyDictionary<string, int> LastReadyForMs => _runtime.LastReadyForMs;

    /// <summary>S141 — atomic immutable runtime snapshot. Safe for background callers.</summary>
    public AutoKeyRuntimeSnapshot SnapshotRuntime() => _runtime.Snapshot();

    /// <summary>
    /// Run one tick synchronously. Mirrors Python's <c>_tick(now)</c>.
    /// Returns the tick interval to wait before the next call (ms).
    /// </summary>
    public int TickOnce()
    {
        if (_disposed) return DefaultTickMs;
        TickCount++;
        var state = _states.Snapshot;
        var author = AutoKeyConfigLoader.AuthorFromState(state);
        var config = AutoKeyConfigLoader.Load(_settings, author);
        var profile = AutoKeyProfileStore.ActiveProfile(config);
        var tickMs = ResolveTickMs(profile);

        if (!config.Enabled || profile is null)
        {
            SetIdle(active: false, profileId: "", reason: "disabled");
            return tickMs;
        }

        SetActiveProfile(profile);

        var engineCfg = profile.Engine;

        if (engineCfg.PauseOnDeath && IsDead(state))
        {
            LastReason = "dead";
            return tickMs;
        }
        if (!(state.RecognitionOk || state.PacketActive))
        {
            LastReason = "recognition-off";
            return tickMs;
        }
        if (engineCfg.RequireForeground && !_foregroundProbe())
        {
            LastReason = "background";
            return tickMs;
        }

        var ctx = BuildContext(state);
        var fired = _runtime.Tick(profile, ctx);
        LastFiredActionId = fired;
        LastReason = fired is null ? "idle" : "fired";
        return tickMs;
    }

    /// <summary>
    /// Start the background tick loop. Idempotent — second call is a
    /// no-op while the first task is still running.
    /// </summary>
    public Task StartAsync(CancellationToken cancellationToken)
    {
        if (_disposed) throw new ObjectDisposedException(nameof(AutoKeyTickHost));
        if (_pump is not null) return Task.CompletedTask;
        _runCts = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token, cancellationToken);
        _pump = Task.Run(() => PumpAsync(_runCts.Token), CancellationToken.None);
        return Task.CompletedTask;
    }

    public async Task StopAsync()
    {
        _shutdown.Cancel();
        _runCts?.Cancel();
        if (_pump is null) return;
        try { await _pump.ConfigureAwait(false); }
        catch (OperationCanceledException) { /* normal */ }
    }

    public void Dispose()
    {
        DisposeAsync().AsTask().GetAwaiter().GetResult();
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed) return;
        _disposed = true;
        _shutdown.Cancel();
        _runCts?.Cancel();
        if (_pump is not null)
        {
            try { await _pump.ConfigureAwait(false); }
            catch (OperationCanceledException) { /* normal */ }
        }
        _runCts?.Dispose();
        _shutdown.Dispose();
    }

    private async Task PumpAsync(CancellationToken cancellationToken)
    {
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                int waitMs;
                try { waitMs = TickOnce(); }
                catch (Exception ex)
                {
                    _log.LogWarning(ex, "[AutoKeyTickHost] tick threw");
                    LastReason = $"error: {ex.GetType().Name}";
                    waitMs = DefaultTickMs;
                }
                if (waitMs < 1) waitMs = 1;
                await Task.Delay(waitMs, cancellationToken).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            // expected on shutdown
        }
    }

    private void SetActiveProfile(AutoKeyProfileSpecRecord profile)
    {
        LastActive = true;
        LastActiveProfileId = profile.Id;
        if (_lastProfileId != profile.Id)
        {
            _lastProfileId = profile.Id;
            _runtime.InvalidateProfileState();
        }
    }

    private void SetIdle(bool active, string profileId, string reason)
    {
        LastActive = active;
        LastActiveProfileId = profileId;
        LastReason = reason;
        LastFiredActionId = null;
        if (!active && _lastProfileId.Length > 0)
        {
            _lastProfileId = "";
            _runtime.InvalidateProfileState();
        }
    }

    private AutoKeySpecContext BuildContext(GameState state)
    {
        var slots = ImmutableDictionary.CreateBuilder<int, SlotReadiness>();
        if (!state.SkillSlots.IsDefault)
        {
            foreach (var s in state.SkillSlots)
            {
                if (s.Index <= 0) continue;
                slots[s.Index] = new SlotReadiness(
                    StateToString(s.State),
                    s.Active,
                    s.ChargeCount,
                    s.RemainingMs,
                    s.CooldownPct);
            }
        }
        return new AutoKeySpecContext(
            HpPct: state.HpPct,
            StaminaPct: state.StaminaPct,
            BurstReady: state.BurstReady,
            ProfessionName: state.ProfessionName ?? "",
            PlayerName: state.PlayerName ?? "",
            InCombat: state.InCombat,
            Slots: slots.ToImmutable(),
            Now: _clock());
    }

    private static bool IsDead(GameState s)
        => s.HpMax > 0 && s.HpCurrent <= 0 && s.HpPct <= 0.001;

    private static string StateToString(SkillSlotState state) => state switch
    {
        SkillSlotState.Ready => "ready",
        SkillSlotState.Active => "active",
        SkillSlotState.Cooldown => "cooldown",
        SkillSlotState.InsufficientEnergy => "insufficient_energy",
        _ => "",
    };

    private static int ResolveTickMs(AutoKeyProfileSpecRecord? profile)
    {
        var ms = profile?.Engine?.TickMs ?? DefaultTickMs;
        if (ms < 10) ms = 10;
        if (ms > 1000) ms = 1000;
        return ms;
    }

    private const int DefaultTickMs = 50;
}
