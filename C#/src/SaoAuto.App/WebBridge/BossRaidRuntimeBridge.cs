using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Narrow runtime controls used by raid_editor.html. The bridge executes
/// real BossRaidEngine operations; if the engine is idle, operations are
/// no-op success just like the defensive Python overlay API.
/// </summary>
public sealed class BossRaidRuntimeBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly BossRaidEngine _engine;
    private readonly SettingsManager? _settings;
    private readonly GameStateManager? _states;
    private readonly string[] _commands;
    private bool _disposed;

    public BossRaidRuntimeBridge(
        BridgeRouter router,
        BossRaidEngine engine,
        SettingsManager? settings = null,
        GameStateManager? states = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _engine = engine ?? throw new ArgumentNullException(nameof(engine));
        _settings = settings;
        _states = states;
        _commands = new[]
        {
            BridgeCommands.StartBossRaid,
            BridgeCommands.StopBossRaid,
            BridgeCommands.RaidNextPhase,
            BridgeCommands.RaidReset,
            BridgeCommands.RaidSetEntityRole,
        };
        _router.Register(BridgeCommands.StartBossRaid, _ => HandleStart());
        _router.Register(BridgeCommands.StopBossRaid, _ => HandleStop());
        _router.Register(BridgeCommands.RaidNextPhase, _ => HandleNextPhase());
        _router.Register(BridgeCommands.RaidReset, _ => HandleReset());
        _router.Register(BridgeCommands.RaidSetEntityRole, HandleSetEntityRole);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var command in _commands)
            _router.Unregister(command);
    }

    private JsonObject HandleStart()
    {
        if (_settings is null)
            return new JsonObject { ["ok"] = false, ["error"] = "settings_unavailable", ["message"] = "BossRaid settings unavailable" };

        var author = CurrentAuthorElement();
        var config = BossRaidConfigStore.Load(_settings, author);
        if (!config.Enabled)
        {
            config = BossRaidConfigStore.Save(_settings, config with { Enabled = true });
        }

        var profile = BossRaidProfile.ActiveProfile(config);
        if (profile is null)
            return new JsonObject { ["ok"] = false, ["message"] = "No active profile" };

        _engine.Start(ToRuntimePhases(profile), profile.EnrageTimeS);
        return Status("start", profile);
    }

    private JsonObject HandleStop()
    {
        _engine.Stop();
        return Status("stop");
    }

    private JsonObject HandleNextPhase()
    {
        _engine.NextPhase();
        return Status("next_phase");
    }

    private JsonObject HandleReset()
    {
        _engine.Stop();
        return Status("reset");
    }

    private JsonObject HandleSetEntityRole(JsonObject? payload)
    {
        var uuid = PayloadInt64(payload, "uuid");
        var role = PayloadString(payload, "role").Trim().ToLowerInvariant();
        if (uuid == 0 || role is not ("boss" or "enemy"))
            return new JsonObject { ["ok"] = false, ["error"] = "bad_payload" };

        if (!_engine.SetEntityRole(uuid, role))
        {
            return new JsonObject
            {
                ["ok"] = false,
                ["error"] = "entity_not_found",
                ["uuid"] = uuid,
                ["role"] = role,
            };
        }

        var status = Status("set_entity_role");
        status["uuid"] = uuid;
        status["role"] = role;
        status["entities"] = SerializeEntities();
        return status;
    }

    private JsonObject Status(string command)
        => Status(command, TryActiveProfile());

    private JsonObject Status(string command, RaidProfile? activeProfile)
    {
        var runtime = RuntimeStatus(activeProfile);
        var status = new JsonObject
        {
            ["ok"] = true,
            ["command"] = command,
            ["running"] = _engine.Running,
            ["phase_idx"] = runtime["phase_idx"]?.GetValue<int>() ?? -1,
            ["phase_name"] = runtime["phase_name"]?.GetValue<string>() ?? string.Empty,
            ["enrage_remaining_s"] = _engine.EnrageRemainingSeconds,
            ["runtime"] = runtime,
        };
        var menuState = BuildMenuState(activeProfile);
        if (menuState is not null)
            status["state"] = menuState;
        return status;
    }

    private JsonObject RuntimeStatus(RaidProfile? activeProfile = null)
    {
        var phase = _engine.CurrentPhase;
        var totalDamage = _engine.TotalDamage;
        var bossTotalHp = activeProfile?.BossTotalHp ?? 0;
        var bossHpPct = bossTotalHp > 0
            ? Math.Max(0.0, 1.0 - (double)totalDamage / bossTotalHp)
            : 0.0;
        return new JsonObject
        {
            ["state"] = _engine.Running ? "running" : "idle",
            ["profile_name"] = activeProfile?.ProfileName ?? string.Empty,
            ["elapsed_s"] = 0,
            ["phase_idx"] = phase?.Index ?? -1,
            ["phase_name"] = phase?.Name ?? string.Empty,
            ["phase_elapsed_s"] = 0,
            ["total_damage"] = totalDamage,
            ["dps"] = 0,
            ["enrage_remaining_s"] = _engine.EnrageRemainingSeconds,
            ["enrage_armed"] = _engine.Running && (activeProfile?.EnrageTimeS ?? 0) > 0,
            ["enrage_urgency"] = string.Empty,
            ["boss_hp_est_pct"] = Math.Round(bossHpPct, 4),
            ["boss_total_hp"] = bossTotalHp,
            ["boss_current_hp"] = bossTotalHp > 0 ? Math.Max(0, bossTotalHp - totalDamage) : 0,
            ["boss_hp_source"] = bossTotalHp > 0 ? "estimate" : "none",
            ["boss_uuid"] = _engine.BossUuid,
            ["boss_invincible"] = _engine.BossInvincible,
        };
    }

    private JsonObject? BuildMenuState(RaidProfile? activeProfile = null)
    {
        if (_settings is null) return null;
        var state = MenuStateBridge.BuildBossRaidState(_settings, _states);
        state["runtime"] = RuntimeStatus(activeProfile);
        return state;
    }

    private RaidProfile? TryActiveProfile()
    {
        if (_settings is null) return null;
        try
        {
            return BossRaidProfile.ActiveProfile(BossRaidConfigStore.Load(_settings, CurrentAuthorElement()));
        }
        catch
        {
            return null;
        }
    }

    private JsonElement CurrentAuthorElement()
    {
        var author = _states is null
            ? AuthorSnapshot.Empty
            : AutoKeyConfigLoader.AuthorFromState(_states.Snapshot);
        var node = new JsonObject
        {
            ["player_uid"] = author.PlayerUid ?? string.Empty,
            ["player_name"] = author.PlayerName ?? string.Empty,
            ["profession_id"] = author.ProfessionId,
            ["profession_name"] = author.ProfessionName ?? string.Empty,
        };
        using var doc = JsonDocument.Parse(node.ToJsonString());
        return doc.RootElement.Clone();
    }

    private static IReadOnlyList<RaidPhase> ToRuntimePhases(RaidProfile profile)
    {
        var phases = new List<RaidPhase>();
        for (int i = 0; i < profile.Phases.Count; i++)
        {
            var phase = profile.Phases[i];
            phases.Add(new RaidPhase(
                Index: i,
                Name: string.IsNullOrWhiteSpace(phase.Name) ? $"P{i + 1}" : phase.Name,
                DurationSeconds: PhaseDurationSeconds(phase),
                Reminder: phase.Timelines.FirstOrDefault()?.Label));
        }
        if (phases.Count == 0)
            phases.Add(new RaidPhase(0, "P1", 0));
        return phases;
    }

    private static double PhaseDurationSeconds(RaidProfilePhase phase)
    {
        if (phase.Timelines.Count == 0) return 0;
        return phase.Timelines.Max(t => Math.Max(0.0, t.TimeSeconds) + Math.Max(0.0, t.DurationS));
    }

    private JsonArray SerializeEntities()
    {
        var entities = new JsonArray();
        foreach (var entity in _engine.Entities)
        {
            entities.Add(new JsonObject
            {
                ["uuid"] = entity.Uuid,
                ["name"] = entity.Name,
                ["role"] = entity.Role,
                ["damage_dealt"] = entity.DamageDealt,
                ["hit_count"] = entity.HitCount,
                ["hp"] = entity.Hp,
                ["max_hp"] = entity.MaxHp,
                ["shield_active"] = entity.ShieldActive,
                ["shield_pct"] = entity.ShieldPct,
                ["breaking_stage"] = entity.BreakingStage,
                ["extinction_pct"] = entity.ExtinctionPct,
                ["in_overdrive"] = entity.InOverdrive,
            });
        }
        return entities;
    }

    private static long PayloadInt64(JsonObject? payload, string key)
    {
        var node = payload?[key];
        if (node is null) return 0;
        try { return node.GetValue<long>(); } catch { /* try other shapes */ }
        try { return node.GetValue<int>(); } catch { /* try double */ }
        try { return (long)node.GetValue<double>(); } catch { /* try string */ }
        try
        {
            var text = node.GetValue<string>();
            return long.TryParse(text, out var parsed) ? parsed : 0;
        }
        catch
        {
            return 0;
        }
    }

    private static string PayloadString(JsonObject? payload, string key)
    {
        var node = payload?[key];
        if (node is null) return string.Empty;
        try { return node.GetValue<string>() ?? string.Empty; }
        catch { return node.ToJsonString(); }
    }
}
