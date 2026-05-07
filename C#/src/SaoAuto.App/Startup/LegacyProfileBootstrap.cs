using Microsoft.Extensions.Logging;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.App.Startup;

/// <summary>
/// S90 — Bootstraps the one-shot legacy profile migration during App startup.
/// Anchored against <see cref="ResourcePathResolver.LegacyProfile"/>
/// (exe-dir <c>player_profile.json</c>, matching Python's <c>_base_dir</c>
/// anchor in <c>character_profile.py</c>).
///
/// Failure here is never fatal — the migration is a convenience for users
/// upgrading from the pre-2026.04 Python build; an unreadable / missing
/// legacy file is the common case, not an error path.
/// </summary>
public static class LegacyProfileBootstrap
{
    public static LegacyProfileMigrator.MigrationResult Run(
        SettingsManager settings,
        ResourcePathResolver resolver,
        ILogger? logger = null)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        if (resolver is null) throw new ArgumentNullException(nameof(resolver));

        var result = LegacyProfileMigrator.Migrate(settings, resolver.LegacyProfile, logger);
        if (logger is not null)
        {
            if (result.Error is not null)
            {
                logger.LogWarning(
                    "[LegacyProfile] migration error at {Path}: {Error}",
                    resolver.LegacyProfile, result.Error);
            }
            else if (result.SettingsChanged)
            {
                logger.LogInformation(
                    "[LegacyProfile] migrated player_profile.json → settings.json (deleted={Deleted})",
                    result.LegacyFileDeleted);
            }
            else if (result.LegacyFileFound)
            {
                logger.LogInformation(
                    "[LegacyProfile] legacy file found but had no useful fields (deleted={Deleted})",
                    result.LegacyFileDeleted);
            }
        }
        return result;
    }
}
