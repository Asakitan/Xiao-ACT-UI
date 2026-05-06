using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Core.Automation;

/// <summary>
/// Top-level orchestrator that wires the deterministic automation
/// engines (<see cref="AutoKeyEngine"/> profile + <see cref="DpsTracker"/> +
/// <see cref="BossRaidEngine"/> + <see cref="ISoundPlayer"/>) onto a
/// <see cref="GameStateManager"/>. Mirrors Python <c>automation.AutomationCore</c>:
/// callers get a single <c>Start/Stop</c> handle and three boolean toggles
/// (Capture / Auto / Sound). Global hotkey wiring lives in the App layer
/// (which owns <c>IGlobalHotkeyService</c>) and just calls
/// <see cref="ToggleCapture"/> / <see cref="ToggleAuto"/> / <see cref="ToggleSound"/>.
/// Live SendInput / packet / recognition wires are injected — the
/// orchestrator itself is testable without a game window.
/// </summary>
public sealed class AutomationCore : IDisposable
{
    private readonly GameStateManager _state;
    private readonly SettingsManager _settings;
    private readonly ISoundPlayer _sound;
    private readonly DpsTracker _dps;
    private readonly BossRaidEngine _boss;
    private readonly AutoKeyCooldownGate _cooldowns = new();
    private readonly Action<KeyStroke>? _emitKey;
    private readonly ILogger _log;
    private readonly object _gate = new();

    private IDisposable? _stateSub;
    private AutoKeyProfile? _profile;
    private bool _running;
    private int _disposed;

    public bool CaptureEnabled { get; private set; } = true;
    public bool AutoEnabled { get; private set; }
    public bool SoundEnabled
    {
        get => _sound.Enabled;
        private set => _sound.Enabled = value;
    }

    /// <summary>Raised after any toggle flips. Caller can refresh UI / persist settings.</summary>
    public event Action<AutomationCore>? StateChanged;

    public DpsTracker Dps => _dps;
    public BossRaidEngine Boss => _boss;
    public ISoundPlayer Sound => _sound;

    public AutomationCore(
        GameStateManager state,
        SettingsManager settings,
        ISoundPlayer? sound = null,
        DpsTracker? dps = null,
        BossRaidEngine? boss = null,
        Action<KeyStroke>? keyEmitter = null,
        ILogger? logger = null)
    {
        _state = state ?? throw new ArgumentNullException(nameof(state));
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _sound = sound ?? new NullSoundPlayer();
        _dps = dps ?? new DpsTracker();
        _boss = boss ?? new BossRaidEngine();
        _emitKey = keyEmitter;
        _log = logger ?? NullLogger.Instance;
    }

    public void LoadProfile(AutoKeyProfile? profile)
    {
        lock (_gate) { _profile = profile; _cooldowns.Reset(); }
    }

    public void Start()
    {
        lock (_gate)
        {
            if (_running) return;
            _running = true;
            _stateSub = _state.Subscribe(OnState);
            _log.LogInformation("[Automation] started capture={Cap} auto={Auto}", CaptureEnabled, AutoEnabled);
        }
    }

    public void Stop()
    {
        lock (_gate)
        {
            if (!_running) return;
            _running = false;
            _stateSub?.Dispose(); _stateSub = null;
            _boss.Stop();
            _log.LogInformation("[Automation] stopped");
        }
    }

    public void ToggleCapture() { CaptureEnabled = !CaptureEnabled; StateChanged?.Invoke(this); }
    public void ToggleAuto()    { AutoEnabled    = !AutoEnabled;    if (!AutoEnabled) _cooldowns.Reset(); StateChanged?.Invoke(this); }
    public void ToggleSound()   { SoundEnabled   = !SoundEnabled;   StateChanged?.Invoke(this); }

    /// <summary>Drive AutoKey from a fresh game-state snapshot. Idempotent + pure-ish.</summary>
    private void OnState(GameState gs)
    {
        if (!_running || !AutoEnabled) return;
        AutoKeyProfile? profile;
        lock (_gate) profile = _profile;
        if (profile is null || !profile.Enabled) return;

        var ctx = new AutoKeyContext(
            HpPct: gs.HpPct,
            StaminaPct: gs.StaminaPct,
            BurstReady: gs.BurstReady,
            BossPhase: gs.BossRaidPhase,
            InCombat: gs.InCombat,
            Now: DateTimeOffset.UtcNow);

        // Highest priority first; fire one matching action per tick (matches Python).
        foreach (var act in profile.Actions.OrderByDescending(a => a.Priority))
        {
            if (!AutoKeyTriggers.ShouldFire(act.Trigger, ctx)) continue;
            if (!_cooldowns.TryFire(act.Id, act.CooldownMs, ctx.Now)) continue;
            try { _emitKey?.Invoke(act.KeyStroke); }
            catch (Exception ex) { _log.LogWarning(ex, "[Automation] key emit failed for {Id}", act.Id); }
            break;
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        Stop();
        _sound.Dispose();
    }
}
