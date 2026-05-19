using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S120 — boss projector pulls Boss* fields off
/// <see cref="GameState.MonsterDataMap"/> when no fresh
/// <see cref="BossHpEvent"/> is winning. Selector prefers the monster
/// aggro'd on the local player (HatedCharId == SelfUuid); fallback is
/// the live Entmonster with the highest MaxHp. Dead rows or
/// MaxHp&lt;=0 are filtered out. The packet path always wins inside
/// the freshness window (5 s); once stale, the projector takes over
/// and BossHpSource flips to MonsterData.
/// </summary>
public class Session120BossProjectorTests
{
    private static EntityAppearance Mon(long uuid, int curHp, int maxHp,
        long hatedCharId = 0, bool inOverdrive = false, int shieldTotal = 0,
        int shieldMaxTotal = 0, int breakingStage = -1)
    {
        return new EntityAppearance(uuid, (int)EEntityType.Entmonster)
        {
            CurHp = curHp,
            MaxHp = maxHp,
            Attrs = new MonsterCoreAttrs
            {
                CurHp = curHp, HasCurHp = true,
                MaxHp = maxHp, HasMaxHp = true,
                InOverdrive = inOverdrive, HasInOverdrive = true,
                ShieldTotal = shieldTotal, ShieldMaxTotal = shieldMaxTotal,
                HasShield = shieldTotal > 0 || shieldMaxTotal > 0,
                BreakingStage = breakingStage,
                HasBreakingStage = breakingStage >= 0,
                HatedCharId = hatedCharId,
                HasHatedCharId = hatedCharId != 0,
            },
        };
    }

    [Fact]
    public void Projector_NoMonsters_LeavesBossUntouched()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            Array.Empty<EntityAppearance>(),
            Array.Empty<EntityDisappearance>(),
            1.0));

        var s = state.Snapshot;
        Assert.Equal(BossHpSource.None, s.BossHpSource);
        Assert.Equal(0, s.BossCurrentHp);
        Assert.Equal(-1, s.BossBreakingStage);
    }

    [Fact]
    public void Projector_SingleLiveMonster_PicksIt()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[] { Mon(7, curHp: 3000, maxHp: 5000, breakingStage: 1) },
            Array.Empty<EntityDisappearance>(),
            1.0));

        var s = state.Snapshot;
        Assert.Equal(BossHpSource.MonsterData, s.BossHpSource);
        Assert.Equal(3000, s.BossCurrentHp);
        Assert.Equal(5000, s.BossTotalHp);
        Assert.Equal(0.6, s.BossHpEstPct, 6);
        Assert.Equal(1, s.BossBreakingStage);
    }

    [Fact]
    public void Projector_HighestMaxHpWins()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                Mon(7, curHp: 100, maxHp: 1000),
                Mon(8, curHp: 5000, maxHp: 50_000),
                Mon(9, curHp: 200, maxHp: 2000),
            },
            Array.Empty<EntityDisappearance>(),
            1.0));

        var s = state.Snapshot;
        Assert.Equal(50_000, s.BossTotalHp);
        Assert.Equal(5000, s.BossCurrentHp);
    }

    [Fact]
    public void Projector_AggroOnSelf_Wins_OverHigherMaxHp()
    {
        var state = new GameStateManager();
        // Pre-seed SelfUuid via EnterGame. SelfUuid must clear the
        // CyCombat.IsPlayerUuid low-marker (≥ 640) so packet routing
        // doesn't drop downstream events; pick a benign player-marker uuid.
        const ulong selfUuid = 1024UL;
        state.Update(s => s with { SelfUuid = selfUuid });
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                Mon(7, curHp: 5000, maxHp: 50_000), // higher MaxHp
                Mon(8, curHp: 1000, maxHp: 10_000, hatedCharId: (long)selfUuid),
            },
            Array.Empty<EntityDisappearance>(),
            1.0));

        var s = state.Snapshot;
        Assert.Equal(10_000, s.BossTotalHp);
        Assert.Equal(1000, s.BossCurrentHp);
    }

    [Fact]
    public void Projector_DeadMonsterFiltered()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[] { Mon(7, curHp: 3000, maxHp: 5000) },
            Array.Empty<EntityDisappearance>(),
            1.0));
        Assert.Equal(BossHpSource.MonsterData, state.Snapshot.BossHpSource);

        // Delta with HasCurHp=true and value 0 flips IsDead.
        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[]
            {
                new NearDeltaAttrUpdate(7, 0, 0, 0) { HasCurHp = true },
            },
        });

        var s = state.Snapshot;
        // No live boss left — projector clears.
        Assert.Equal(BossHpSource.None, s.BossHpSource);
        Assert.Equal(0, s.BossCurrentHp);
        Assert.Equal(0, s.BossTotalHp);
        Assert.Equal(-1, s.BossBreakingStage);
    }

    [Fact]
    public void Projector_PacketPathWinsWithinFreshnessWindow()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new BossHpEvent(
            CurrentHp: 1234, MaxHp: 9999, EstimatedPct: 0.1234,
            ShieldActive: false, ShieldPct: 0.0, BreakingStage: 2,
            InOverdrive: true, Invincible: false,
            TimestampSeconds: 100.0));
        Assert.Equal(BossHpSource.Packet, state.Snapshot.BossHpSource);

        // Apply near-entities 1 second later (well inside 5-s window).
        bridge.Apply(new NearEntitiesEvent(
            new[] { Mon(7, curHp: 5000, maxHp: 50_000) },
            Array.Empty<EntityDisappearance>(),
            101.0));

        var s = state.Snapshot;
        Assert.Equal(BossHpSource.Packet, s.BossHpSource);
        Assert.Equal(1234, s.BossCurrentHp);
        Assert.Equal(9999, s.BossTotalHp);
    }

    [Fact]
    public void Projector_TakesOverWhenPacketPathStale()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new BossHpEvent(
            CurrentHp: 1234, MaxHp: 9999, EstimatedPct: 0.1234,
            ShieldActive: false, ShieldPct: 0.0, BreakingStage: 2,
            InOverdrive: true, Invincible: false,
            TimestampSeconds: 100.0));

        // Delta arrives 6 s later — past the 5-s freshness window.
        bridge.Apply(new NearEntitiesEvent(
            new[] { Mon(7, curHp: 5000, maxHp: 50_000, breakingStage: 0) },
            Array.Empty<EntityDisappearance>(),
            106.0));

        var s = state.Snapshot;
        Assert.Equal(BossHpSource.MonsterData, s.BossHpSource);
        Assert.Equal(5000, s.BossCurrentHp);
        Assert.Equal(50_000, s.BossTotalHp);
        Assert.Equal(0, s.BossBreakingStage);
    }
}
