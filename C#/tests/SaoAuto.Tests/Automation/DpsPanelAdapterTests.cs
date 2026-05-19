using System.Collections.Immutable;
using System.Text.Json;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

public class DpsPanelAdapterTests
{
    [Fact]
    public void EmptySnapshotProducesInactivePayload()
    {
        var snap = new DpsTracker().Snapshot();
        var payload = DpsPanelAdapter.Build(snap);
        Assert.False(payload.EncounterActive);
        Assert.Empty(payload.Entities);
        Assert.Equal(0, payload.TotalDamage);
    }

    [Fact]
    public void DamagePctRoundedToThreeDecimals()
    {
        var t = new DpsTracker();
        t.RecordDamage(1, "A", 700, 1, true);
        t.RecordDamage(2, "B", 300, 2, false);
        var payload = DpsPanelAdapter.Build(t.Snapshot());

        Assert.Equal(2, payload.Entities.Count);
        // Sorted by damage descending → A first.
        Assert.Equal(0.7, payload.Entities[0].DamagePct);
        Assert.Equal(0.3, payload.Entities[1].DamagePct);
    }

    [Fact]
    public void DamagePctZeroWhenNoDamage()
    {
        var t = new DpsTracker();
        t.RecordHeal(1, "Healer", 500, 1, false);
        var payload = DpsPanelAdapter.Build(t.Snapshot());
        Assert.True(payload.EncounterActive);
        Assert.Equal(0.0, payload.Entities[0].DamagePct);
        Assert.Equal(500, payload.Entities[0].HealTotal);
    }

    [Fact]
    public void EncounterTimestampsPassedThrough()
    {
        var snap = new DpsTracker().Snapshot();
        var payload = DpsPanelAdapter.Build(snap, encounterStartedAt: 100.0, encounterEndedAt: 105.5);
        Assert.Equal(100.0, payload.EncounterStartedAt);
        Assert.Equal(105.5, payload.EncounterEndedAt);
    }

    [Fact]
    public void ElapsedSRoundedToOneDecimal()
    {
        // Synthesise a snapshot with non-trivial duration via record builder.
        var snap = new DpsSnapshot(
            Active: true, TotalDamage: 1000, Dps: 500,
            TotalHeal: 0, Hps: 0,
            DurationSeconds: 2.137,
            Rows: ImmutableArray<DpsEntitySnapshot>.Empty);
        var payload = DpsPanelAdapter.Build(snap);
        Assert.Equal(2.1, payload.ElapsedS);
    }

    [Fact]
    public void ToJsonEmitsSnakeCaseKeys()
    {
        var t = new DpsTracker();
        t.RecordDamage(1, "Self", 1000, 12, true);
        var json = DpsPanelAdapter.ToJson(t.Snapshot());

        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;
        Assert.True(root.TryGetProperty("encounter_active", out _));
        Assert.True(root.TryGetProperty("elapsed_s", out _));
        Assert.True(root.TryGetProperty("total_damage", out _));
        Assert.True(root.TryGetProperty("total_dps", out _));
        Assert.True(root.TryGetProperty("entities", out var entities));
        Assert.Equal(JsonValueKind.Array, entities.ValueKind);
        var first = entities[0];
        Assert.True(first.TryGetProperty("uid", out _));
        Assert.True(first.TryGetProperty("damage_total", out _));
        Assert.True(first.TryGetProperty("damage_pct", out _));
        Assert.True(first.TryGetProperty("is_self", out _));
        Assert.True(first.TryGetProperty("profession_id", out _));
    }

    [Fact]
    public void EntitiesHandleDefaultSkillsImmutableArray()
    {
        // Snapshot() (no skills) leaves Skills as default ImmutableArray<>.
        var t = new DpsTracker();
        t.RecordDamage(1, "x", 50, 1, true);
        var payload = DpsPanelAdapter.Build(t.Snapshot());
        Assert.NotNull(payload.Entities[0].Skills);
        Assert.Empty(payload.Entities[0].Skills);
    }

    [Fact]
    public void SkillsForwardedWhenSnapshotIncludesThem()
    {
        var t = new DpsTracker();
        t.RecordDamage(1, "Self", 200, 1, true, skillId: 7, skillName: "Slash");
        var payload = DpsPanelAdapter.Build(t.SnapshotWithSkills());
        Assert.NotEmpty(payload.Entities[0].Skills);
        Assert.Equal(7, payload.Entities[0].Skills[0].SkillId);
    }
}
