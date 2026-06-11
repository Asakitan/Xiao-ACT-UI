using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;

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
    private readonly string[] _commands;
    private bool _disposed;

    public BossRaidRuntimeBridge(BridgeRouter router, BossRaidEngine engine)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _engine = engine ?? throw new ArgumentNullException(nameof(engine));
        _commands = new[] { BridgeCommands.RaidNextPhase, BridgeCommands.RaidReset, BridgeCommands.RaidSetEntityRole };
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
    {
        var phase = _engine.CurrentPhase;
        return new JsonObject
        {
            ["ok"] = true,
            ["command"] = command,
            ["running"] = _engine.Running,
            ["phase_idx"] = phase?.Index ?? -1,
            ["phase_name"] = phase?.Name ?? string.Empty,
            ["enrage_remaining_s"] = _engine.EnrageRemainingSeconds,
        };
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
