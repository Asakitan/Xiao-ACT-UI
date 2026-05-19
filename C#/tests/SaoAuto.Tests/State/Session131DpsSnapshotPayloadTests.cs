using System.Collections.Immutable;
using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.State;

/// <summary>
/// S131 — Pins the DPS rollup keys added to
/// <see cref="StateSnapshotPayload.ToDict(GameState, DpsSnapshot?)"/>:
/// total damage/heal/HPS scalars plus <c>dps_entries</c> rows with
/// optional per-skill breakdowns.
/// </summary>
public class Session131DpsSnapshotPayloadTests
{
    [Fact]
    public void NullDps_EmitsZeroDefaultsAndEmptyEntries()
    {
        var d = StateSnapshotPayload.ToDict(new GameState(), dps: null);

        Assert.False(d["dps_active"]!.GetValue<bool>());
        Assert.Equal(0L, d["dps_total_damage"]!.GetValue<long>());
        Assert.Equal(0L, d["dps_total"]!.GetValue<long>());
        Assert.Equal(0L, d["dps_total_heal"]!.GetValue<long>());
        Assert.Equal(0L, d["dps_hps"]!.GetValue<long>());
        Assert.Equal(0.0, d["dps_duration_seconds"]!.GetValue<double>());
        Assert.Null(d["dps_report_reason"]);

        var entries = d["dps_entries"] as JsonArray;
        Assert.NotNull(entries);
        Assert.Empty(entries!);
    }

    [Fact]
    public void Snapshot_PassesThroughTotalsRowsAndSkillBreakdown()
    {
        var snap = new DpsSnapshot(
            Active: true,
            TotalDamage: 1_000,
            Dps: 500,
            TotalHeal: 200,
            Hps: 100,
            DurationSeconds: 2.5,
            Rows: ImmutableArray.Create(
                new DpsEntitySnapshot(
                    EntityUuid: 11,
                    EntityName: "Asuna",
                    ProfessionId: 7,
                    IsSelf: true,
                    Damage: 700,
                    Dps: 350,
                    Heal: 150,
                    Hps: 75,
                    Skills: ImmutableArray.Create(
                        new SkillBreakdownRow(
                            SkillId: 42,
                            Name: "Slash",
                            Total: 500,
                            Hits: 3,
                            CritHits: 1,
                            CritRate: 1.0 / 3.0,
                            MaxHit: 300,
                            HealTotal: 50,
                            HealHits: 1))),
                new DpsEntitySnapshot(
                    EntityUuid: 22,
                    EntityName: "Kirito",
                    ProfessionId: 8,
                    IsSelf: false,
                    Damage: 300,
                    Dps: 150,
                    Heal: 50,
                    Hps: 25)),
            ReportReason: "idle_timeout");

        var d = StateSnapshotPayload.ToDict(new GameState(), snap);

        Assert.True(d["dps_active"]!.GetValue<bool>());
        Assert.Equal(1_000L, d["dps_total_damage"]!.GetValue<long>());
        Assert.Equal(500L, d["dps_total"]!.GetValue<long>());
        Assert.Equal(200L, d["dps_total_heal"]!.GetValue<long>());
        Assert.Equal(100L, d["dps_hps"]!.GetValue<long>());
        Assert.Equal(2.5, d["dps_duration_seconds"]!.GetValue<double>());
        Assert.Equal("idle_timeout", d["dps_report_reason"]!.GetValue<string>());

        var entries = (d["dps_entries"] as JsonArray)!;
        Assert.Equal(2, entries.Count);

        var first = (entries[0] as JsonObject)!;
        Assert.Equal(11L, first["uid"]!.GetValue<long>());
        Assert.Equal("Asuna", first["name"]!.GetValue<string>());
        Assert.Equal(7, first["profession_id"]!.GetValue<int>());
        Assert.True(first["is_self"]!.GetValue<bool>());
        Assert.Equal(700L, first["damage_total"]!.GetValue<long>());
        Assert.Equal(150L, first["heal_total"]!.GetValue<long>());
        Assert.Equal(350L, first["dps"]!.GetValue<long>());
        Assert.Equal(75L, first["hps"]!.GetValue<long>());
        Assert.Equal(0.7, first["damage_pct"]!.GetValue<double>());

        var skills = (first["skills"] as JsonArray)!;
        Assert.Single(skills);
        var skill = (skills[0] as JsonObject)!;
        Assert.Equal(42, skill["skill_id"]!.GetValue<int>());
        Assert.Equal("Slash", skill["name"]!.GetValue<string>());
        Assert.Equal(500L, skill["total"]!.GetValue<long>());
        Assert.Equal(3, skill["hits"]!.GetValue<int>());
        Assert.Equal(1, skill["crit_hits"]!.GetValue<int>());
        Assert.Equal(300L, skill["max_hit"]!.GetValue<long>());
        Assert.Equal(50L, skill["heal_total"]!.GetValue<long>());
        Assert.Equal(1, skill["heal_hits"]!.GetValue<int>());

        var secondSkills = (entries[1]!["skills"] as JsonArray)!;
        Assert.Empty(secondSkills);
    }
}
