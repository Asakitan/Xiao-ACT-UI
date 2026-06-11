using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S167 — Registers <c>dps.reset_combat</c>,
/// <c>dps.show_last_report</c>, and <c>dps.entity_detail</c> on a <see cref="BridgeRouter"/>,
/// backed by thin delegates so the runner site can wire whatever
/// <see cref="DpsTracker"/> is live without coupling the bridge to
/// the packet-runtime lifetime. When the delegates are <c>null</c>
/// (e.g. packet runtime never started), commands return a structured
/// <c>{error}</c> instead of throwing or no-oping silently.
///
/// Reply shapes:
/// <list type="bullet">
/// <item><c>dps.reset_combat</c> ← (no payload) →
///   <c>{reset:true}</c> or <c>{error:"dps_unavailable"}</c>.</item>
/// <item><c>dps.show_last_report</c> ← (no payload) →
///   <c>{report:string, duration_s:number, total_damage:number}</c>
///   or <c>{error:"no_report"|"dps_unavailable"}</c>.</item>
/// <item><c>dps.entity_detail</c> ← <c>{uid:number}</c> →
///   <c>{uid,name,damage_total,skills:[...]}</c>
///   or <c>{error:"not_found"|"dps_unavailable"}</c>.</item>
/// </list>
/// </summary>
public sealed class DpsBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly Action? _reset;
    private readonly Func<DpsSnapshot?>? _lastReport;
    private readonly SettingsManager? _settings;
    private readonly DpsTracker? _tracker;
    private bool _disposed;

    public DpsBridge(
        BridgeRouter router,
        Action? reset,
        Func<DpsSnapshot?>? lastReport,
        SettingsManager? settings = null,
        DpsTracker? tracker = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _reset = reset;
        _lastReport = lastReport;
        _settings = settings;
        _tracker = tracker;
        router.Register(BridgeCommands.ResetCombat, _ => HandleReset());
        router.Register(BridgeCommands.ShowLastDpsReport, _ => HandleLastReport());
        router.Register(BridgeCommands.DpsEntityDetail, HandleEntityDetail);
        // R8 / DPS-04: toggle command always registers; null settings just
        // surfaces {error:"settings_unavailable"} per the existing
        // dps_unavailable pattern.
        router.Register(BridgeCommands.DpsToggleEnabled, HandleToggle);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _router.Unregister(BridgeCommands.ResetCombat);
        _router.Unregister(BridgeCommands.ShowLastDpsReport);
        _router.Unregister(BridgeCommands.DpsEntityDetail);
        _router.Unregister(BridgeCommands.DpsToggleEnabled);
    }

    private JsonObject HandleToggle(JsonObject? payload)
    {
        if (_settings is null) return new JsonObject { ["error"] = "settings_unavailable" };
        // Payload `{enabled:bool}` writes; absent / wrong shape just reads.
        var current = _settings.Get<bool?>(SettingsKeys.DpsEnabled) ?? true;
        if (payload is not null
            && payload.TryGetPropertyValue("enabled", out var node)
            && node is not null)
        {
            try
            {
                var next = node.GetValue<bool>();
                if (next != current)
                {
                    _settings.Set(SettingsKeys.DpsEnabled, next);
                    _settings.Save();
                    current = next;
                }
            }
            catch (Exception)
            {
                return new JsonObject { ["error"] = "bad_payload" };
            }
        }
        return new JsonObject { ["enabled"] = current };
    }

    private JsonObject HandleReset()
    {
        if (_reset is null) return new JsonObject { ["error"] = "dps_unavailable" };
        _reset();
        return new JsonObject { ["reset"] = true };
    }

    private JsonObject HandleLastReport()
    {
        if (_lastReport is null) return new JsonObject { ["error"] = "dps_unavailable" };
        var snap = _lastReport();
        if (snap is null) return new JsonObject { ["error"] = "no_report" };
        return new JsonObject
        {
            ["report"] = DpsTracker.FormatReport(snap),
            ["duration_s"] = snap.DurationSeconds,
            ["total_damage"] = snap.TotalDamage,
        };
    }

    private JsonObject HandleEntityDetail(JsonObject? payload)
    {
        if (_tracker is null) return new JsonObject { ["error"] = "dps_unavailable" };
        var uid = PayloadInt64(payload, "uid");
        if (uid == 0) return new JsonObject { ["error"] = "missing_uid" };

        var snap = _tracker.SnapshotWithSkills();
        var row = snap.Rows.FirstOrDefault(r => r.EntityUuid == uid);
        if (row is null) return new JsonObject { ["error"] = "not_found", ["uid"] = uid };
        return SerializeEntityDetail(row, snap.TotalDamage, snap.DurationSeconds);
    }

    private static long PayloadInt64(JsonObject? payload, string key)
    {
        var node = payload?[key];
        if (node is null) return 0;
        try { return node.GetValue<long>(); } catch { /* try other shapes */ }
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

    private static JsonObject SerializeEntityDetail(
        DpsEntitySnapshot row,
        long totalDamage,
        double elapsedSeconds)
    {
        var skills = SerializeSkills(row.Skills);
        var damageHits = 0;
        var critHits = 0;
        long maxHit = 0;
        if (!row.Skills.IsDefaultOrEmpty)
        {
            foreach (var skill in row.Skills)
            {
                damageHits += skill.Hits;
                critHits += skill.CritHits;
                if (skill.MaxHit > maxHit) maxHit = skill.MaxHit;
            }
        }

        return new JsonObject
        {
            ["uid"] = row.EntityUuid,
            ["name"] = row.EntityName,
            ["profession_id"] = row.ProfessionId,
            ["profession"] = row.ProfessionId > 0 ? row.ProfessionId.ToString() : string.Empty,
            ["is_self"] = row.IsSelf,
            ["damage_total"] = row.Damage,
            ["heal_total"] = row.Heal,
            ["dps"] = row.Dps,
            ["hps"] = row.Hps,
            ["damage_pct"] = totalDamage > 0
                ? Math.Round((double)row.Damage / totalDamage, 3)
                : 0.0,
            ["elapsed_s"] = elapsedSeconds,
            ["damage_hits"] = damageHits,
            ["crit_rate"] = damageHits > 0 ? (double)critHits / damageHits : 0.0,
            ["max_hit"] = maxHit,
            ["skills"] = skills,
        };
    }

    private static JsonArray SerializeSkills(System.Collections.Immutable.ImmutableArray<SkillBreakdownRow> skills)
    {
        var arr = new JsonArray();
        if (skills.IsDefaultOrEmpty) return arr;
        foreach (var skill in skills)
        {
            arr.Add(new JsonObject
            {
                ["skill_id"] = skill.SkillId,
                ["name"] = skill.Name,
                ["skill_name"] = skill.Name,
                ["total"] = skill.Total,
                ["hits"] = skill.Hits,
                ["crit_hits"] = skill.CritHits,
                ["crit_rate"] = skill.CritRate,
                ["max_hit"] = skill.MaxHit,
                ["heal_total"] = skill.HealTotal,
                ["heal_hits"] = skill.HealHits,
            });
        }
        return arr;
    }
}
