namespace SaoAuto.Core.Automation;

/// <summary>
/// Name → file-path lookup for SAO sound clips. Mirrors Python's
/// <c>SAO_SOUNDS</c> dict + <c>play_sound(name, ...)</c> resolver. The
/// catalog keeps the WAV root directory so callers can reference clips
/// by short name (<c>"click"</c>, <c>"alert"</c>, …) without leaking
/// filesystem paths into the call sites.
/// </summary>
public sealed class SoundCatalog
{
    /// <summary>Default short-name → relative-filename map (subset of Python's <c>SAO_SOUNDS</c>).</summary>
    public static readonly IReadOnlyDictionary<string, string> Defaults =
        new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["click"]        = "click.wav",
            ["menu_open"]    = "menu_open.wav",
            ["menu_close"]   = "menu_close.wav",
            ["panel"]        = "panel.wav",
            ["submenu"]      = "submenu.wav",
            ["alert"]        = "alert.wav",
            ["alert_close"]  = "alert_close.wav",
            ["welcome"]      = "welcome.wav",
            ["alo_welcome"]  = "alo_welcome.wav",
            ["link_start"]   = "link_start.wav",
            ["nervegear"]    = "nervegear.wav",
            ["burst_ready"]  = "burst_ready.wav",
            ["boss_alert"]   = "boss_alert.wav",
            ["boss_phase"]   = "boss_phase.wav",
            ["levelup"]      = "levelup.wav",
        };

    private readonly Dictionary<string, string> _map;
    private readonly string _root;

    public SoundCatalog(string rootDirectory, IReadOnlyDictionary<string, string>? overrides = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(rootDirectory);
        _root = rootDirectory;
        _map = new Dictionary<string, string>(Defaults, StringComparer.OrdinalIgnoreCase);
        if (overrides is not null)
        {
            foreach (var kv in overrides) _map[kv.Key] = kv.Value;
        }
    }

    public string RootDirectory => _root;

    /// <summary>Register or replace a name → relative-filename mapping.</summary>
    public void Set(string name, string relativeFile)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        ArgumentException.ThrowIfNullOrEmpty(relativeFile);
        _map[name] = relativeFile;
    }

    /// <summary>Resolve a short name to an absolute path. Returns null when the name is unknown.</summary>
    public string? Resolve(string name)
    {
        if (string.IsNullOrEmpty(name)) return null;
        if (!_map.TryGetValue(name, out var rel)) return null;
        return Path.IsPathRooted(rel) ? rel : Path.Combine(_root, rel);
    }

    public IReadOnlyCollection<string> Names => _map.Keys;
}

/// <summary>
/// Multi-stage playback orchestrator for the level-up SFX. C# port of
/// <c>sao_sound.play_levelup_sfx</c>: fires the level-up clip on the main
/// channel and (optionally) chains a small celebratory sequence on a
/// background timer. The visual side (<c>LevelUpEffect.show</c>) is left
/// to the WPF shell; this class owns audio orchestration only.
/// </summary>
public sealed class LevelUpEffect
{
    private readonly ISoundPlayer _player;
    private readonly SoundCatalog _catalog;

    public LevelUpEffect(ISoundPlayer player, SoundCatalog catalog)
    {
        _player = player ?? throw new ArgumentNullException(nameof(player));
        _catalog = catalog ?? throw new ArgumentNullException(nameof(catalog));
    }

    /// <summary>
    /// Trigger the level-up effect. Plays <c>"levelup"</c> immediately, then
    /// — if defined — chains a follow-up <c>"burst_ready"</c> clip after a
    /// short delay. Returns the names actually played (in order). Returns
    /// empty when the player is disabled.
    /// </summary>
    public IReadOnlyList<string> Play(int oldLevel, int newLevel, IDelayScheduler? scheduler = null)
    {
        if (!_player.Enabled) return Array.Empty<string>();
        if (newLevel <= oldLevel) return Array.Empty<string>();

        var played = new List<string>();
        var primary = _catalog.Resolve("levelup");
        if (primary is not null) { _player.Play(primary); played.Add("levelup"); }

        var follow = _catalog.Resolve("burst_ready");
        if (follow is not null)
        {
            var sched = scheduler ?? ImmediateScheduler.Instance;
            sched.Schedule(TimeSpan.FromMilliseconds(450), () =>
            {
                if (_player.Enabled) _player.Play(follow);
            });
            played.Add("burst_ready");
        }
        return played;
    }
}

/// <summary>Indirection for the level-up follow-up delay so tests run synchronously.</summary>
public interface IDelayScheduler
{
    void Schedule(TimeSpan delay, Action action);
}

public sealed class ImmediateScheduler : IDelayScheduler
{
    public static readonly ImmediateScheduler Instance = new();
    public void Schedule(TimeSpan delay, Action action) => action();
}
