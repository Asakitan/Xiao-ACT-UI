using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Core.Automation;

/// <summary>
/// S144 — instance-level facade over <see cref="AutoKeyProfileStore"/>
/// + <see cref="AutoKeyConfigLoader"/> for callers that want a single
/// thread-safe load-mutate-save surface (UI profile editor, web
/// bridge, CLI commands). Pure composition over the existing static
/// helpers; no new on-disk format. Mutations serialize on
/// <see cref="SettingsManager.Save"/>, which is also taken when the
/// service writes through.
///
/// Mirrors the role of Python's GUI <c>auto_key_engine</c> helper
/// methods that wrap <c>upsert_profile</c> / <c>delete_profile</c> /
/// <c>clone_profile</c> with a `save_auto_key_config` call.
/// </summary>
public sealed class AutoKeyProfileService
{
    private readonly SettingsManager _settings;
    private readonly GameStateManager? _states;
    private readonly Func<string>? _newId;
    private readonly Func<DateTimeOffset>? _clock;
    private readonly object _gate = new();

    public AutoKeyProfileService(
        SettingsManager settings,
        GameStateManager? states = null,
        Func<string>? newId = null,
        Func<DateTimeOffset>? clock = null)
    {
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _states = states;
        _newId = newId;
        _clock = clock;
    }

    /// <summary>Load the live config (state-aware author fallback).</summary>
    public AutoKeyConfig Load()
    {
        lock (_gate)
        {
            return LoadLocked();
        }
    }

    /// <summary>
    /// Insert-or-replace <paramref name="profile"/>. When
    /// <paramref name="activate"/> is true, also flips the active id.
    /// </summary>
    public AutoKeyConfig Upsert(AutoKeyProfileSpecRecord profile, bool activate = false)
    {
        ArgumentNullException.ThrowIfNull(profile);
        lock (_gate)
        {
            var cfg = LoadLocked();
            cfg = AutoKeyProfileStore.UpsertProfile(cfg, profile, activate);
            SaveLocked(cfg);
            return cfg;
        }
    }

    /// <summary>Remove a profile; promotes the first remaining as active when needed.</summary>
    public AutoKeyConfig Delete(string profileId)
    {
        if (string.IsNullOrEmpty(profileId)) return Load();
        lock (_gate)
        {
            var cfg = LoadLocked();
            var next = AutoKeyProfileStore.DeleteProfile(cfg, profileId);
            if (!ReferenceEquals(next, cfg)) SaveLocked(next);
            return next;
        }
    }

    /// <summary>
    /// Clone a profile by id; returns null when the source id is
    /// absent. The clone is upserted (not activated) and persisted.
    /// </summary>
    public AutoKeyProfileSpecRecord? Clone(string profileId, AuthorSnapshot? authorOverride = null)
    {
        if (string.IsNullOrEmpty(profileId)) return null;
        lock (_gate)
        {
            var cfg = LoadLocked();
            var (next, cloned) = AutoKeyProfileStore.CloneProfile(cfg, profileId, authorOverride, _newId, _clock);
            if (cloned is null) return null;
            SaveLocked(next);
            return cloned;
        }
    }

    /// <summary>Switch the active profile id; no-op when the id is absent.</summary>
    public AutoKeyConfig SetActive(string profileId)
    {
        lock (_gate)
        {
            var cfg = LoadLocked();
            if (string.IsNullOrEmpty(profileId) || AutoKeyProfileStore.FindProfile(cfg, profileId) is null)
                return cfg;
            if (cfg.ActiveProfileId == profileId) return cfg;
            var next = cfg with { ActiveProfileId = profileId };
            SaveLocked(next);
            return next;
        }
    }

    /// <summary>Flip the global enabled flag.</summary>
    public AutoKeyConfig SetEnabled(bool enabled)
    {
        lock (_gate)
        {
            var cfg = LoadLocked();
            if (cfg.Enabled == enabled) return cfg;
            var next = cfg with { Enabled = enabled };
            SaveLocked(next);
            return next;
        }
    }

    private AutoKeyConfig LoadLocked()
    {
        var author = _states is not null
            ? AutoKeyConfigLoader.AuthorFromState(_states.Snapshot)
            : (AuthorSnapshot?)null;
        return AutoKeyConfigLoader.Load(_settings, author, _newId, _clock);
    }

    private void SaveLocked(AutoKeyConfig cfg)
    {
        AutoKeyConfigLoader.Save(_settings, cfg);
        _settings.Save();
    }
}
