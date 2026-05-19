using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Bridge;

/// <summary>
/// S113 — skeleton port of Python's <c>MonsterData</c>. The
/// <see cref="GameState.MonsterDataMap"/> table lazy-creates a row on
/// each Entmonster appearance, mirrors HP/MaxHp/Level updates from
/// <see cref="NearDeltaEvent"/>, and removes the row on disappearance.
/// Crucially, a delta carrying real HP=0 (S112 HasCurHp=true) flips
/// <see cref="MonsterData.IsDead"/> — closing the death-detection
/// loop the entity table by itself can't represent. Name / TemplateId
/// land in S114 when the AttrCollection extractor grows the NAME / ID
/// branches.
/// </summary>
public class Session113MonsterDataTests
{
    private const int EntmonsterType = (int)EEntityType.Entmonster;

    [Fact]
    public void AppearMonster_LazyCreatesMonsterData()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, EntmonsterType)
                {
                    CurHp = 5_000, MaxHp = 8_000, Level = 12,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.Equal(7, mon.Uuid);
        Assert.Equal(7L & 0xFFFFFFFFL, mon.Uid);
        Assert.Equal(5_000, mon.Hp);
        Assert.Equal(8_000, mon.MaxHp);
        Assert.Equal(12, mon.Level);
        Assert.Equal(-1, mon.BreakingStage); // sentinel: "not received"
        Assert.False(mon.IsDead);
        Assert.Equal(string.Empty, mon.Name); // S114 fills this in
        Assert.Equal(0, mon.TemplateId);
        Assert.Equal(1.0, mon.LastUpdateSeconds);
    }

    [Fact]
    public void AppearNonMonster_NoMonsterDataRow()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        // Anything not Entmonster (player, pet, NPC, errtype) gets an
        // entity-table row but no monster-data row.
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, 0), // Enterrtype
                new EntityAppearance(8, 2), // some non-monster type
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        Assert.Empty(state.Snapshot.MonsterDataMap);
        Assert.Equal(2, state.Snapshot.NearEntities.Count);
    }

    [Fact]
    public void AppearMonster_FirstSeenWins()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[] { new EntityAppearance(7, EntmonsterType) { CurHp = 1_000, MaxHp = 1_000 } },
            Array.Empty<EntityDisappearance>(), 1.0));
        // Second appearance must NOT clobber the first.
        bridge.Apply(new NearEntitiesEvent(
            new[] { new EntityAppearance(7, EntmonsterType) { CurHp = 9_999, MaxHp = 9_999 } },
            Array.Empty<EntityDisappearance>(), 2.0));

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.Equal(1_000, mon.Hp);
        Assert.Equal(1.0, mon.LastUpdateSeconds);
    }

    [Fact]
    public void Delta_UpdatesMonsterDataHp()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[] { new EntityAppearance(7, EntmonsterType) { CurHp = 5_000, MaxHp = 8_000 } },
            Array.Empty<EntityDisappearance>(), 1.0));

        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[]
            {
                new NearDeltaAttrUpdate(7, CurHp: 3_500, MaxHp: 0, Level: 0)
                {
                    HasCurHp = true,
                },
            },
        });

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.Equal(3_500, mon.Hp);
        Assert.Equal(8_000, mon.MaxHp); // carry-forward
        Assert.False(mon.IsDead);
        Assert.Equal(2.0, mon.LastUpdateSeconds);
    }

    [Fact]
    public void Delta_RealZeroHp_FlipsIsDead()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[] { new EntityAppearance(7, EntmonsterType) { CurHp = 5_000, MaxHp = 8_000 } },
            Array.Empty<EntityDisappearance>(), 1.0));

        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[]
            {
                new NearDeltaAttrUpdate(7, CurHp: 0, MaxHp: 0, Level: 0)
                {
                    HasCurHp = true,
                },
            },
        });

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.Equal(0, mon.Hp);
        Assert.True(mon.IsDead);
    }

    [Fact]
    public void Delta_LegacyZero_DoesNotFlipIsDead()
    {
        // Without HasCurHp, a CurHp=0 means "absent" under the legacy
        // rule — must NOT mark dead.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[] { new EntityAppearance(7, EntmonsterType) { CurHp = 5_000 } },
            Array.Empty<EntityDisappearance>(), 1.0));

        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[] { new NearDeltaAttrUpdate(7, 0, 0, 0) },
        });

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.False(mon.IsDead);
        Assert.Equal(5_000, mon.Hp);
    }

    [Fact]
    public void DisappearRemovesMonsterData()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[] { new EntityAppearance(7, EntmonsterType) { CurHp = 1 } },
            Array.Empty<EntityDisappearance>(), 1.0));
        Assert.Single(state.Snapshot.MonsterDataMap);

        bridge.Apply(new NearEntitiesEvent(
            Array.Empty<EntityAppearance>(),
            new[] { new EntityDisappearance(7, 0) }, 2.0));
        Assert.Empty(state.Snapshot.MonsterDataMap);
        Assert.Empty(state.Snapshot.NearEntities);
    }
}
