using System.Collections.Immutable;
using System.Text.Json.Nodes;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.State;

/// <summary>
/// S130 — Pins the new <c>monster_table</c> array key on
/// <see cref="StateSnapshotPayload.ToDict"/>: per-uuid records from
/// <see cref="GameState.MonsterDataMap"/> sorted by MaxHp desc, with
/// the full S113/S114-S117/S123/S128 attr surface (HP / break gauge /
/// shield / death flags / aggro / BuffList).
/// </summary>
public class Session130MonsterTableTests
{
    [Fact]
    public void EmptyMap_EmitsEmptyArray()
    {
        var d = StateSnapshotPayload.ToDict(new GameState());
        var arr = d["monster_table"] as JsonArray;
        Assert.NotNull(arr);
        Assert.Empty(arr!);
    }

    [Fact]
    public void SortedByMaxHpDesc_ThenUuid()
    {
        var s = new GameState
        {
            MonsterDataMap = ImmutableDictionary<long, MonsterData>.Empty
                .Add(101, new MonsterData { Uuid = 101, MaxHp = 5_000, Name = "Trash" })
                .Add(202, new MonsterData { Uuid = 202, MaxHp = 50_000, Name = "Boss" })
                .Add(303, new MonsterData { Uuid = 303, MaxHp = 5_000, Name = "Mob" }),
        };
        var arr = (StateSnapshotPayload.ToDict(s)["monster_table"] as JsonArray)!;
        Assert.Equal(3, arr.Count);
        Assert.Equal(202, arr[0]!["uuid"]!.GetValue<long>());
        // Tie on MaxHp resolved by uuid asc.
        Assert.Equal(101, arr[1]!["uuid"]!.GetValue<long>());
        Assert.Equal(303, arr[2]!["uuid"]!.GetValue<long>());
    }

    [Fact]
    public void AllMonsterFields_PassedThrough()
    {
        var s = new GameState
        {
            MonsterDataMap = ImmutableDictionary<long, MonsterData>.Empty
                .Add(42, new MonsterData
                {
                    Uuid = 42, Uid = 7, Name = "Skogul", TemplateId = 999,
                    Hp = 12345, MaxHp = 50000, Level = 80,
                    BreakingStage = 2, Extinction = 100, MaxExtinction = 500,
                    Stunned = 50, MaxStunned = 200,
                    InOverdrive = true,
                    ShieldActive = true, ShieldTotal = 1000, ShieldMaxTotal = 2000,
                    IsDead = false,
                    IsLockStunned = true, StopBreakingTicking = true,
                    State = 3, DeadType = 0, DeadTime = 0,
                    FirstAttack = true,
                    HatedCharId = 9001, HatedCharName = "Hero",
                    LastUpdateSeconds = 12.5,
                }),
        };
        var row = ((StateSnapshotPayload.ToDict(s)["monster_table"] as JsonArray)![0] as JsonObject)!;
        Assert.Equal(42, row["uuid"]!.GetValue<long>());
        Assert.Equal(7, row["uid"]!.GetValue<long>());
        Assert.Equal("Skogul", row["name"]!.GetValue<string>());
        Assert.Equal(999, row["template_id"]!.GetValue<int>());
        Assert.Equal(12345, row["hp"]!.GetValue<int>());
        Assert.Equal(50000, row["max_hp"]!.GetValue<int>());
        Assert.Equal(80, row["level"]!.GetValue<int>());
        Assert.Equal(2, row["breaking_stage"]!.GetValue<int>());
        Assert.Equal(100, row["extinction"]!.GetValue<int>());
        Assert.Equal(500, row["max_extinction"]!.GetValue<int>());
        Assert.Equal(50, row["stunned"]!.GetValue<int>());
        Assert.Equal(200, row["max_stunned"]!.GetValue<int>());
        Assert.True(row["in_overdrive"]!.GetValue<bool>());
        Assert.True(row["shield_active"]!.GetValue<bool>());
        Assert.Equal(1000, row["shield_total"]!.GetValue<int>());
        Assert.Equal(2000, row["shield_max_total"]!.GetValue<int>());
        Assert.False(row["is_dead"]!.GetValue<bool>());
        Assert.True(row["is_lock_stunned"]!.GetValue<bool>());
        Assert.True(row["stop_breaking_ticking"]!.GetValue<bool>());
        Assert.Equal(3, row["state"]!.GetValue<int>());
        Assert.True(row["first_attack"]!.GetValue<bool>());
        Assert.Equal(9001, row["hated_char_id"]!.GetValue<long>());
        Assert.Equal("Hero", row["hated_char_name"]!.GetValue<string>());
        Assert.Equal(12.5, row["last_update_seconds"]!.GetValue<double>());
    }

    [Fact]
    public void DefaultMonster_BreakingStageMinusOne()
    {
        // S113 sentinel: BreakingStage defaults to -1 ("not received yet")
        // not 0 — preserved through to JSON so JS can distinguish.
        var s = new GameState
        {
            MonsterDataMap = ImmutableDictionary<long, MonsterData>.Empty
                .Add(1, new MonsterData { Uuid = 1, MaxHp = 100 }),
        };
        var row = ((StateSnapshotPayload.ToDict(s)["monster_table"] as JsonArray)![0] as JsonObject)!;
        Assert.Equal(-1, row["breaking_stage"]!.GetValue<int>());
    }

    [Fact]
    public void BuffList_SerializedAsArray()
    {
        var s = new GameState
        {
            MonsterDataMap = ImmutableDictionary<long, MonsterData>.Empty
                .Add(1, new MonsterData
                {
                    Uuid = 1, MaxHp = 100,
                    BuffList = ImmutableArray.Create(
                        new BuffEntry { Id = 555, DurationMs = 8000 },
                        new BuffEntry { Id = 777, DurationMs = 3000 }),
                }),
        };
        var row = ((StateSnapshotPayload.ToDict(s)["monster_table"] as JsonArray)![0] as JsonObject)!;
        var buffs = row["buff_list"] as JsonArray;
        Assert.NotNull(buffs);
        Assert.Equal(2, buffs!.Count);
    }

    [Fact]
    public void DeadMonster_StillIncluded()
    {
        // Dead rows kept so per-encounter death tracking can observe the
        // transition; consumer filters with `!is_dead` if it wants live only.
        var s = new GameState
        {
            MonsterDataMap = ImmutableDictionary<long, MonsterData>.Empty
                .Add(1, new MonsterData { Uuid = 1, MaxHp = 100, IsDead = true, Hp = 0 }),
        };
        var arr = (StateSnapshotPayload.ToDict(s)["monster_table"] as JsonArray)!;
        Assert.Single(arr);
        Assert.True((arr[0] as JsonObject)!["is_dead"]!.GetValue<bool>());
    }

    [Fact]
    public void MonsterTableKey_PresentOnDefaultState()
    {
        // Always emitted so JS subscribers never need to null-guard.
        var d = StateSnapshotPayload.ToDict(new GameState());
        Assert.True(d.ContainsKey("monster_table"));
    }
}
