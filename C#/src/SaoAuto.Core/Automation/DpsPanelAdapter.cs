using System.Text.Json;
using System.Text.Json.Serialization;

namespace SaoAuto.Core.Automation;

/// <summary>
/// WebView / WPF panel-payload adapter — converts a
/// <see cref="DpsSnapshot"/> into the snake_case JSON shape that
/// the Python <c>dps_tracker._build_snapshot_locked</c> emits
/// (encounter_active / elapsed_s / total_damage / total_dps /
/// entities[]). Mirrors the dict-shape consumed by the existing
/// HTML/JS panel so the C# tracker can drop into the same
/// rendering path with no front-end change.
/// </summary>
public static class DpsPanelAdapter
{
    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        WriteIndented = false,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    /// <summary>Build the strongly-typed payload from a snapshot.
    /// <paramref name="encounterStartedAt"/> / <paramref name="encounterEndedAt"/>
    /// are the monotonic-second window bounds (default 0 = "not tracked").
    /// </summary>
    public static DpsPanelPayload Build(
        DpsSnapshot snap,
        double encounterStartedAt = 0.0,
        double encounterEndedAt = 0.0)
    {
        ArgumentNullException.ThrowIfNull(snap);
        var totalDmg = snap.TotalDamage;
        var entities = new List<DpsPanelEntity>(snap.Rows.Length);
        foreach (var row in snap.Rows)
        {
            entities.Add(new DpsPanelEntity(
                Uid: row.EntityUuid,
                Name: row.EntityName,
                ProfessionId: row.ProfessionId,
                IsSelf: row.IsSelf,
                DamageTotal: row.Damage,
                HealTotal: row.Heal,
                Dps: row.Dps,
                Hps: row.Hps,
                DamagePct: totalDmg > 0 ? Math.Round((double)row.Damage / totalDmg, 3) : 0.0,
                Skills: row.Skills.IsDefault ? Array.Empty<SkillBreakdownRow>() : row.Skills));
        }

        return new DpsPanelPayload(
            EncounterActive: snap.Active,
            EncounterStartedAt: encounterStartedAt,
            EncounterEndedAt: encounterEndedAt,
            ElapsedS: Math.Round(snap.DurationSeconds, 1),
            TotalDamage: totalDmg,
            TotalDamageAll: totalDmg,
            TotalHeal: snap.TotalHeal,
            TotalDps: snap.Dps,
            TotalHps: snap.Hps,
            Entities: entities);
    }

    /// <summary>Convenience: serialize <see cref="Build"/> to a JSON
    /// string suitable for `webview.evalScript("update(" + json + ")")`.</summary>
    public static string ToJson(DpsSnapshot snap, double encounterStartedAt = 0.0, double encounterEndedAt = 0.0)
        => JsonSerializer.Serialize(Build(snap, encounterStartedAt, encounterEndedAt), Options);
}

public sealed record DpsPanelPayload(
    bool EncounterActive,
    double EncounterStartedAt,
    double EncounterEndedAt,
    double ElapsedS,
    long TotalDamage,
    long TotalDamageAll,
    long TotalHeal,
    long TotalDps,
    long TotalHps,
    IReadOnlyList<DpsPanelEntity> Entities);

public sealed record DpsPanelEntity(
    long Uid,
    string Name,
    int ProfessionId,
    bool IsSelf,
    long DamageTotal,
    long HealTotal,
    long Dps,
    long Hps,
    double DamagePct,
    IReadOnlyList<SkillBreakdownRow> Skills);
